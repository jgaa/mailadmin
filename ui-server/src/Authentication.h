#pragma once
#include <chrono>
#include <filesystem>
#include <map>
#include <mutex>
#include <string>

namespace mailadmin::ui {
enum class LoginStatus { accepted, rejected, rate_limited };
struct LoginResult { LoginStatus status; std::string revision; };

class Authentication {
public:
    Authentication(std::filesystem::path credentials_file, std::filesystem::path initial_password_file,
                   std::string default_username = "admin");
    // Returns true only when bootstrap credentials were created/recovered.
    bool initialize();
    void set_password(std::string username, std::string password);
    LoginResult verify(const std::string& username, const std::string& password, const std::string& address);
    bool current(const std::string& revision) const;
    std::string username() const;
    const std::filesystem::path& initial_password_file() const { return initial_password_file_; }
    static std::string password_file(const std::filesystem::path& path);
private:
    struct Attempts { std::chrono::steady_clock::time_point started; unsigned count = 0; };
    std::filesystem::path credentials_file_;
    std::filesystem::path initial_password_file_;
    std::string default_username_;
    std::mutex mutex_;
    std::map<std::string, Attempts> attempts_;
};
}
