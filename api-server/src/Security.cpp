#include "Security.h"
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <crypt.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <algorithm>
#include <cerrno>
#include <iostream>
#include <stdexcept>

namespace mailadmin {
namespace {
struct Fd {
    int value;
    explicit Fd(int value) : value{value} { if (value < 0) throw std::runtime_error{"Key file operation failed"}; }
    ~Fd() { close(value); }
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
};
void write_all(int fd, std::string_view bytes) {
    while (!bytes.empty()) {
        const auto count = write(fd, bytes.data(), bytes.size());
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) throw std::runtime_error{"Cannot write key verifier"};
        bytes.remove_prefix(static_cast<std::size_t>(count));
    }
    if (fsync(fd)) throw std::runtime_error{"Cannot persist key verifier"};
}
}
Digest digest(std::string_view secret) {
    Digest result{};
    unsigned size = 0;
    if (EVP_Digest(secret.data(), secret.size(), result.data(), &size, EVP_sha256(), nullptr) != 1
        || size != result.size()) throw std::runtime_error{"Digest generation failed"};
    return result;
}
std::string hex(std::span<const unsigned char> bytes) {
    constexpr std::string_view alphabet = "0123456789abcdef";
    std::string result;
    result.reserve(bytes.size() * 2);
    for (const auto value : bytes) {
        result += alphabet[value >> 4];
        result += alphabet[value & 15];
    }
    return result;
}
std::string random_secret() {
    std::array<unsigned char, 32> bytes{};
    if (RAND_bytes(bytes.data(), static_cast<int>(bytes.size())) != 1)
        throw std::runtime_error{"Secure random generation failed"};
    auto result = hex(bytes);
    OPENSSL_cleanse(bytes.data(), bytes.size());
    return result;
}
void create_key(const std::filesystem::path& path, bool rotate) {
    if (!path.is_absolute()) throw std::runtime_error{"Key path must be absolute"};
    // The existing state directory must belong to this service and exclude
    // other writers. No implicit directory creation or permissive fallback.
    Fd directory{open(path.parent_path().c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW)};
    struct stat info{};
    if (fstat(directory.value, &info) || info.st_uid != geteuid() || (info.st_mode & 0022))
        throw std::runtime_error{"Key directory must be service-owned and not writable by others"};
    if (rotate) static_cast<void>(load_key(path));
    auto token = std::string{"ma_"} + random_secret();
    const auto verifier = hex(digest(token)) + "\n";
    const auto temporary = std::string{".key-"} + random_secret();
    const auto filename = path.filename().string();
    Fd file{openat(directory.value, temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600)};
    try {
        write_all(file.value, verifier);
        if (rotate) {
            if (renameat(directory.value, temporary.c_str(), directory.value, filename.c_str()))
                throw std::runtime_error{"Cannot replace key verifier"};
        } else {
            if (linkat(directory.value, temporary.c_str(), directory.value, filename.c_str(), 0))
                throw std::runtime_error{"Cannot create key verifier (already exists or inaccessible)"};
            if (unlinkat(directory.value, temporary.c_str(), 0))
                throw std::runtime_error{"Cannot remove temporary key verifier"};
        }
        if (fsync(directory.value)) throw std::runtime_error{"Cannot persist key directory"};
    } catch (...) {
        unlinkat(directory.value, temporary.c_str(), 0);
        OPENSSL_cleanse(token.data(), token.size());
        throw;
    }
    // This is deliberately CLI output, never a logger call.
    std::cout << token << '\n' << std::flush;
    OPENSSL_cleanse(token.data(), token.size());
    if (!std::cout) throw std::runtime_error{"Key saved but CLI output failed; rotate it again"};
}
Digest load_key(const std::filesystem::path& path) {
    Fd file{open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK)};
    struct stat info{};
    if (fstat(file.value, &info) || !S_ISREG(info.st_mode) || info.st_size != 65
        || !((info.st_uid == geteuid() && !(info.st_mode & 0077))
            || (info.st_uid == 0 && (geteuid() == 0 || info.st_gid == getegid()) && !(info.st_mode & 0037))))
        throw std::runtime_error{"Key verifier must be service-owned, mode 0600 and valid"};
    if (info.st_uid == 0 && geteuid() != 0) {
        // Root-owned content is insufficient if Courier can replace its parent
        // directory. Every ancestor must be root-owned and exclude writers.
        auto parent = path.parent_path();
        for (;;) {
            struct stat ancestor{};
            if (lstat(parent.c_str(), &ancestor) || !S_ISDIR(ancestor.st_mode) || ancestor.st_uid != 0 || (ancestor.st_mode & 0022))
                throw std::runtime_error{"Unsafe root-owned key directory"};
            if (parent == "/") break;
            parent = parent.parent_path();
        }
    }
    std::array<char, 65> bytes{};
    std::size_t offset = 0;
    while (offset != bytes.size()) {
        const auto count = read(file.value, bytes.data() + offset, bytes.size() - offset);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) throw std::runtime_error{"Cannot read key verifier"};
        offset += static_cast<std::size_t>(count);
    }
    if (bytes.back() != '\n') throw std::runtime_error{"Invalid key verifier"};
    auto digit = [](char value) -> unsigned char {
        if (value >= '0' && value <= '9') return value - '0';
        if (value >= 'a' && value <= 'f') return value - 'a' + 10;
        throw std::runtime_error{"Invalid key verifier"};
    };
    Digest result{};
    for (std::size_t index = 0; index != result.size(); ++index)
        result[index] = static_cast<unsigned char>((digit(bytes[2 * index]) << 4) | digit(bytes[2 * index + 1]));
    return result;
}
bool verify_key(std::string_view authorization, const Digest& expected) {
    constexpr std::string_view prefix = "Bearer ma_";
    if (!authorization.starts_with(prefix) || authorization.size() != prefix.size() + 64) return false;
    auto supplied = digest(authorization.substr(7));
    const bool matches = CRYPTO_memcmp(supplied.data(), expected.data(), expected.size()) == 0;
    OPENSSL_cleanse(supplied.data(), supplied.size());
    return matches;
}
std::string password_hash(std::string_view password) {
    if (password.empty() || password.size() > 1024 || password.find('\0') != std::string_view::npos)
        throw std::runtime_error{"Invalid password length or encoding"};
    // Courier authlib accepts system crypt(3) hashes. OpenSSL supplies the salt;
    // libxcrypt supplies the SHA-512 crypt format supported by Debian 12/13.
    const auto salt = "$6$rounds=100000$" + random_secret().substr(0, 16) + "$";
    std::string input{password};
    crypt_data state{};
    const auto* result = crypt_r(input.c_str(), salt.c_str(), &state);
    std::string hash = result ? result : "";
    OPENSSL_cleanse(input.data(), input.size());
    OPENSSL_cleanse(&state, sizeof(state));
    if (!hash.starts_with(salt)) throw std::runtime_error{"Password hashing failed"};
    return hash;
}
bool password_matches(std::string_view password, std::string_view hash) {
    if (password.find('\0') != std::string_view::npos || hash.empty()) return false;
    std::string input{password}, setting{hash};
    crypt_data state{};
    const auto* result = crypt_r(input.c_str(), setting.c_str(), &state);
    const bool matches = result && std::string_view{result}.size() == hash.size()
        && CRYPTO_memcmp(result, hash.data(), hash.size()) == 0;
    OPENSSL_cleanse(input.data(), input.size());
    OPENSSL_cleanse(&state, sizeof(state));
    return matches;
}
}
