#include "Authentication.h"
#include <boost/json.hpp>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <array>
#include <cerrno>
#include <fcntl.h>
#include <stdexcept>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

namespace mailadmin::ui {
namespace {
constexpr int iterations = 600000;
using Bytes = std::array<unsigned char,32>;
struct Fd {
    int value;
    explicit Fd(int value) : value{value} { if (value < 0) throw std::runtime_error{"Cannot open UI authentication storage"}; }
    ~Fd() { close(value); }
    Fd(const Fd&) = delete;
};
struct Wipe { std::string& value; ~Wipe() { OPENSSL_cleanse(value.data(),value.size()); } };
std::string random_hex() {
    Bytes bytes{};
    if (RAND_bytes(bytes.data(),bytes.size()) != 1) throw std::runtime_error{"Secure password generation failed"};
    std::string value;
    for (const auto byte : bytes) { value += "0123456789abcdef"[byte >> 4]; value += "0123456789abcdef"[byte & 15]; }
    OPENSSL_cleanse(bytes.data(),bytes.size());
    return value;
}
std::string encode(const Bytes& bytes) {
    std::string value;
    for (const auto byte : bytes) { value += "0123456789abcdef"[byte >> 4]; value += "0123456789abcdef"[byte & 15]; }
    return value;
}
Bytes decode(const std::string& text) {
    if (text.size() != 64) throw std::runtime_error{"Invalid UI authentication hash"};
    auto digit = [](char c) -> unsigned {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        throw std::runtime_error{"Invalid UI authentication hash"};
    };
    Bytes value{};
    for (std::size_t index = 0; index < value.size(); ++index) value[index] = (digit(text[index * 2]) << 4) | digit(text[index * 2 + 1]);
    return value;
}
Bytes derive(const std::string& password, const Bytes& salt) {
    Bytes value{};
    if (PKCS5_PBKDF2_HMAC(password.data(),static_cast<int>(password.size()),salt.data(),salt.size(),iterations,EVP_sha256(),value.size(),value.data()) != 1)
        throw std::runtime_error{"UI password hashing failed"};
    return value;
}
void valid_password(const std::string& value) {
    if (value.size() < 12 || value.size() > 1024 || value.find_first_of(std::string{"\0\r\n",3}) != std::string::npos)
        throw std::runtime_error{"UI passwords must contain 12–1024 bytes without NUL or line breaks"};
}
void valid_username(const std::string& value) {
    if (value.empty() || value.size() > 64) throw std::runtime_error{"Invalid UI username"};
    for (const unsigned char c : value)
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.'))
            throw std::runtime_error{"Invalid UI username"};
}
std::string read_file(const std::filesystem::path& path, bool missing = false) {
    const int raw = open(path.c_str(),O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (raw < 0 && missing && errno == ENOENT) return {};
    Fd fd{raw};
    struct stat info{};
    if (fstat(fd.value,&info) || !S_ISREG(info.st_mode) || (info.st_mode & 0077)
        || (info.st_uid != 0 && info.st_uid != geteuid()) || info.st_size < 1 || info.st_size > 4096)
        throw std::runtime_error{"UI authentication files must be protected regular files with mode 0600"};
    std::string value(info.st_size,'\0');
    std::size_t offset = 0;
    while (offset < value.size()) {
        const auto count = read(fd.value,value.data() + offset,value.size() - offset);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) throw std::runtime_error{"Cannot read UI authentication storage"};
        offset += count;
    }
    return value;
}
Fd directory(const std::filesystem::path& file) {
    auto parent = file.parent_path();
    if (parent.empty()) parent = ".";
    if (!std::filesystem::exists(parent)) {
        std::filesystem::create_directories(parent);
        if (chmod(parent.c_str(),0700)) throw std::runtime_error{"Cannot protect UI authentication directory"};
    }
    Fd fd{open(parent.c_str(),O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC)};
    struct stat info{};
    if (fstat(fd.value,&info) || (info.st_mode & 0022) || (info.st_uid != 0 && info.st_uid != geteuid()))
        throw std::runtime_error{"Unsafe UI authentication directory"};
    return Fd{dup(fd.value)};
}
void write_file(const std::filesystem::path& path, const std::string& content, bool replace) {
    auto parent = directory(path);
    const auto temporary = ".mailadmin-auth-" + random_hex();
    try {
        Fd fd{openat(parent.value,temporary.c_str(),O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC,0600)};
        std::size_t offset = 0;
        while (offset < content.size()) {
            const auto count = write(fd.value,content.data() + offset,content.size() - offset);
            if (count < 0 && errno == EINTR) continue;
            if (count <= 0) throw std::runtime_error{"Cannot write UI authentication storage"};
            offset += count;
        }
        if (fsync(fd.value)) throw std::runtime_error{"Cannot persist UI authentication storage"};
        if (replace) {
            if (renameat(parent.value,temporary.c_str(),parent.value,path.filename().c_str())) throw std::runtime_error{"Cannot publish UI authentication hash"};
        } else {
            if (linkat(parent.value,temporary.c_str(),parent.value,path.filename().c_str(),0)) throw std::runtime_error{"Initial password file already exists or cannot be published"};
            if (unlinkat(parent.value,temporary.c_str(),0)) throw std::runtime_error{"Cannot finish initial password publication"};
        }
        if (fsync(parent.value)) throw std::runtime_error{"Cannot persist UI authentication directory"};
    } catch (...) { unlinkat(parent.value,temporary.c_str(),0); throw; }
}
Fd lock_file(const std::filesystem::path& file) {
    auto parent = directory(file);
    const auto name = file.filename().string() + ".lock";
    Fd lock{openat(parent.value,name.c_str(),O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC,0600)};
    struct stat info{};
    if (fstat(lock.value,&info) || !S_ISREG(info.st_mode) || (info.st_mode & 0077)
        || (info.st_uid != 0 && info.st_uid != geteuid()) || flock(lock.value,LOCK_EX))
        throw std::runtime_error{"Cannot lock UI authentication storage"};
    return Fd{dup(lock.value)};
}
boost::json::object record(const std::filesystem::path& file) {
    boost::system::error_code error;
    auto value = boost::json::parse(read_file(file),error);
    if (error || !value.is_object()) throw std::runtime_error{"Invalid UI authentication file"};
    const auto& object = value.as_object();
    if (object.at("version") != 1 || object.at("algorithm") != "pbkdf2-sha256" || object.at("iterations") != iterations)
        throw std::runtime_error{"Unsupported UI authentication hash"};
    valid_username(std::string{object.at("username").as_string()});
    (void)decode(std::string{object.at("salt").as_string()});
    (void)decode(std::string{object.at("hash").as_string()});
    return object;
}
void save(const std::filesystem::path& file, const std::string& username, const std::string& password) {
    valid_username(username); valid_password(password);
    const auto salt_text = random_hex();
    const auto salt = decode(salt_text);
    auto hash = derive(password,salt);
    const auto output = boost::json::serialize(boost::json::object{{"version",1},{"username",username},
        {"algorithm","pbkdf2-sha256"},{"iterations",iterations},{"salt",salt_text},{"hash",encode(hash)}});
    OPENSSL_cleanse(hash.data(),hash.size());
    write_file(file,output,true);
}
}

Authentication::Authentication(std::filesystem::path credentials_file, std::filesystem::path initial_password_file, std::string default_username)
    : credentials_file_{std::move(credentials_file)}, initial_password_file_{std::move(initial_password_file)}, default_username_{std::move(default_username)} {
    if (credentials_file_.empty() || initial_password_file_.empty() || std::filesystem::absolute(credentials_file_).lexically_normal() == std::filesystem::absolute(initial_password_file_).lexically_normal())
        throw std::runtime_error{"Separate UI hash and initial-password paths are required"};
    valid_username(default_username_);
}
bool Authentication::initialize() {
    auto lock = lock_file(credentials_file_);
    if (!read_file(credentials_file_,true).empty()) { (void)record(credentials_file_); return false; }
    auto password = read_file(initial_password_file_,true);
    Wipe wipe{password};
    if (password.empty()) {
        password = random_hex();
        write_file(initial_password_file_,password + "\n",false);
    } else {
        if (password.ends_with('\n')) password.pop_back();
        if (password.ends_with('\r')) password.pop_back();
    }
    save(credentials_file_,default_username_,password);
    return true;
}
void Authentication::set_password(std::string username, std::string password) {
    Wipe wipe{password};
    auto lock = lock_file(credentials_file_);
    save(credentials_file_,username,password);
}
std::string Authentication::password_file(const std::filesystem::path& file) {
    auto value = read_file(file);
    if (value.ends_with('\n')) value.pop_back();
    if (value.ends_with('\r')) value.pop_back();
    valid_password(value);
    return value;
}
std::string Authentication::username() const { return std::string{record(credentials_file_).at("username").as_string()}; }
bool Authentication::current(const std::string& revision) const {
    try { return std::string{record(credentials_file_).at("salt").as_string()} == revision; } catch (...) { return false; }
}
LoginResult Authentication::verify(const std::string& username, const std::string& password, const std::string& address) {
    const auto now = std::chrono::steady_clock::now();
    {
        std::lock_guard lock{mutex_};
        std::erase_if(attempts_,[now](const auto& item) { return now - item.second.started >= std::chrono::minutes{1}; });
        if (!attempts_.contains(address) && attempts_.size() >= 1024) return {LoginStatus::rate_limited,{}};
        auto [position, inserted] = attempts_.try_emplace(address,Attempts{now,0});
        if (position->second.count >= 5) return {LoginStatus::rate_limited,{}};
        ++position->second.count;
    }
    if (password.size() > 1024 || password.find('\0') != std::string::npos) return {LoginStatus::rejected,{}};
    const auto data = record(credentials_file_);
    const auto salt = std::string{data.at("salt").as_string()};
    auto actual = derive(password,decode(salt));
    const auto expected = decode(std::string{data.at("hash").as_string()});
    const bool password_matches = CRYPTO_memcmp(actual.data(),expected.data(),actual.size()) == 0;
    OPENSSL_cleanse(actual.data(),actual.size());
    if (!password_matches || std::string{data.at("username").as_string()} != username) return {LoginStatus::rejected,{}};
    { std::lock_guard lock{mutex_}; attempts_.erase(address); }
    return {LoginStatus::accepted,salt};
}
}
