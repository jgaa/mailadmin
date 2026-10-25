#include "Credentials.h"
#include <openssl/crypto.h>
#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <fcntl.h>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>

namespace mailadmin::ui {
Credential::Credential(std::string value) : value_{std::move(value)} {
    if (value_.empty() || value_.size() > 512 || !std::ranges::all_of(value_, [](unsigned char c) {
            return c >= 33 && c <= 126;
        })) {
        OPENSSL_cleanse(value_.data(), value_.size());
        throw std::runtime_error{"The configured API key must be nonempty printable ASCII without whitespace"};
    }
}
Credential::~Credential() { OPENSSL_cleanse(value_.data(), value_.size()); }

std::shared_ptr<const Credential> load_credential(const std::filesystem::path& file) {
    auto selected_file = file;
    if (selected_file.empty()) {
        if (const auto* path = std::getenv("MAILADMIN_API_KEY_FILE")) selected_file = path;
    }
    std::string key;
    if (const auto* value = std::getenv("MAILADMIN_API_KEY")) key = value;
    struct Wipe { std::string& key; ~Wipe() { OPENSSL_cleanse(key.data(), key.size()); } } wipe{key};
    if (!key.empty() && !selected_file.empty())
        throw std::runtime_error{"Configure either MAILADMIN_API_KEY or an API key file, not both"};
    if (!selected_file.empty()) {
        const int fd = open(selected_file.c_str(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) throw std::runtime_error{"Cannot open the configured API key file"};
        struct Close { int fd; ~Close() { close(fd); } } close_fd{fd};
        struct stat info{};
        if (fstat(fd, &info) || !S_ISREG(info.st_mode) || (info.st_mode & 0037)
            || (info.st_uid != 0 && info.st_uid != geteuid()) || info.st_size < 1 || info.st_size > 514)
            throw std::runtime_error{"API key file must be a protected regular file owned by root or the service user, without group write or other access"};
        key.resize(static_cast<std::size_t>(info.st_size));
        std::size_t offset = 0;
        while (offset < key.size()) {
            const auto count = read(fd, key.data() + offset, key.size() - offset);
            if (count < 0 && errno == EINTR) continue;
            if (count <= 0) throw std::runtime_error{"Cannot read the configured API key file"};
            offset += static_cast<std::size_t>(count);
        }
        while (!key.empty() && (key.back() == '\n' || key.back() == '\r')) key.pop_back();
    }
    if (key.empty()) throw std::runtime_error{"Set MAILADMIN_API_KEY, MAILADMIN_API_KEY_FILE or --api-key-file before starting the UI"};
    auto credential = std::make_shared<const Credential>(key);
    // Avoid passing this service credential to subsequently spawned children.
    // Container environment metadata remains visible to the container operator.
    unsetenv("MAILADMIN_API_KEY");
    return credential;
}
}
