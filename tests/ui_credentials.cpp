#include "Credentials.h"
#include <openssl/rand.h>
#include <array>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>

namespace {
void require(bool condition) { if (!condition) throw std::runtime_error{"Credential check failed"}; }
void rejects(const std::function<void()>& action, const std::string& secret) {
    bool rejected = false;
    try { action(); } catch (const std::exception& error) {
        require(std::string{error.what()}.find(secret) == std::string::npos);
        rejected = true;
    }
    require(rejected);
}
}
int main() {
    std::filesystem::path directory;
    try {
        unsetenv("MAILADMIN_API_KEY"); unsetenv("MAILADMIN_API_KEY_FILE");
        std::array<unsigned char,32> random{};
        require(RAND_bytes(random.data(),random.size()) == 1);
        std::string secret;
        for (const auto byte : random) { secret += "0123456789abcdef"[byte >> 4]; secret += "0123456789abcdef"[byte & 15]; }
        rejects([] { (void)mailadmin::ui::load_credential(); },secret);
        setenv("MAILADMIN_API_KEY",secret.c_str(),1);
        require(mailadmin::ui::load_credential()->value() == secret);
        require(std::getenv("MAILADMIN_API_KEY") == nullptr);
        char temporary[] = "/tmp/mailadmin-ui-credential-XXXXXX";
        require(mkdtemp(temporary) != nullptr); directory = temporary;
        const auto file = directory / "key";
        { std::ofstream output{file}; output << secret << "\r\n"; }
        require(chmod(file.c_str(),0600) == 0);
        require(mailadmin::ui::load_credential(file)->value() == secret);
        setenv("MAILADMIN_API_KEY_FILE",file.c_str(),1);
        require(mailadmin::ui::load_credential()->value() == secret);
        setenv("MAILADMIN_API_KEY",secret.c_str(),1);
        rejects([&] { (void)mailadmin::ui::load_credential(file); },secret);
        unsetenv("MAILADMIN_API_KEY"); unsetenv("MAILADMIN_API_KEY_FILE");
        require(chmod(file.c_str(),0640) == 0);
        require(mailadmin::ui::load_credential(file)->value() == secret);
        require(chmod(file.c_str(),0644) == 0);
        rejects([&] { (void)mailadmin::ui::load_credential(file); },secret);
        require(chmod(file.c_str(),0660) == 0);
        rejects([&] { (void)mailadmin::ui::load_credential(file); },secret);
        require(chmod(file.c_str(),0600) == 0);
        std::filesystem::create_symlink(file,directory / "link");
        rejects([&] { (void)mailadmin::ui::load_credential(directory / "link"); },secret);
        require(mkfifo((directory / "fifo").c_str(),0600) == 0);
        rejects([&] { (void)mailadmin::ui::load_credential(directory / "fifo"); },secret);
        setenv("MAILADMIN_API_KEY_FILE","/does/not/exist",1);
        require(mailadmin::ui::load_credential(file)->value() == secret);
        unsetenv("MAILADMIN_API_KEY_FILE");
        { std::ofstream output{file}; output << secret << " invalid"; }
        rejects([&] { (void)mailadmin::ui::load_credential(file); },secret);
        std::filesystem::remove_all(directory);
        std::cout << "Server credential source and file-boundary checks passed\n";
    } catch (...) {
        if (!directory.empty()) std::filesystem::remove_all(directory);
        std::cerr << "Server credential check failed\n";
        return 1;
    }
}
