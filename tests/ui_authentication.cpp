#include "Authentication.h"
#include <fstream>
#include <iostream>
#include <sys/stat.h>
#include <unistd.h>
using namespace mailadmin::ui;
namespace {
void require(bool condition) { if (!condition) throw std::runtime_error{"Authentication check failed"}; }
template<typename Function> void rejects(Function function) {
    bool rejected = false;
    try { function(); } catch (...) { rejected = true; }
    require(rejected);
}
std::string contents(const std::filesystem::path& file) {
    std::ifstream stream{file};
    return {std::istreambuf_iterator<char>{stream},std::istreambuf_iterator<char>{}};
}
void mode(const std::filesystem::path& file) {
    struct stat info{};
    require(stat(file.c_str(),&info) == 0 && (info.st_mode & 0777) == 0600);
}
}
int main() {
    char name[] = "/tmp/mailadmin-ui-auth-XXXXXX";
    if (!mkdtemp(name)) return 1;
    const std::filesystem::path directory{name};
    try {
        const auto hash = directory / "admin.json", initial = directory / "initial-password";
        Authentication authentication{hash,initial};
        require(authentication.initialize()); mode(hash); mode(initial);
        auto password = Authentication::password_file(initial);
        require(password.size() == 64 && contents(hash).find(password) == std::string::npos);
        auto result = authentication.verify("admin",password,"client");
        require(result.status == LoginStatus::accepted && authentication.current(result.revision));
        require(authentication.verify("other",password,"client").status == LoginStatus::rejected);
        require(authentication.verify("admin",password + "wrong","client").status == LoginStatus::rejected);
        require(!authentication.initialize() && Authentication::password_file(initial) == password);
        authentication.set_password("admin",password + "rotated");
        require(!authentication.current(result.revision));
        require(authentication.verify("admin",password,"client").status == LoginStatus::rejected);
        require(authentication.verify("admin",password + "rotated","client").status == LoginStatus::accepted);
        for (unsigned count = 0; count < 5; ++count)
            require(authentication.verify("admin","wrong","blocked").status == LoginStatus::rejected);
        require(authentication.verify("admin",password + "rotated","blocked").status == LoginStatus::rate_limited);
        rejects([&] { authentication.set_password("admin","short"); });
        require(authentication.verify("admin",password + "rotated","client").status == LoginStatus::accepted);
        // Recover bootstrap interrupted between plaintext publication and hash publication.
        std::filesystem::remove(hash);
        require(authentication.initialize());
        require(authentication.verify("admin",password,"client").status == LoginStatus::accepted);
        chmod(initial.c_str(),0644);
        rejects([&] { Authentication::password_file(initial); });
        chmod(initial.c_str(),0600);
        std::filesystem::create_symlink(initial,directory / "symlink");
        rejects([&] { Authentication::password_file(directory / "symlink"); });
        chmod(hash.c_str(),0644);
        rejects([&] { authentication.initialize(); });
        require(!authentication.current(result.revision));
        chmod(hash.c_str(),0600);
        Authentication custom{directory / "custom.json",directory / "custom-initial","operator"};
        custom.set_password("operator",password);
        require(!std::filesystem::exists(directory / "custom-initial"));
        require(custom.username() == "operator" && custom.verify("operator",password,"client").status == LoginStatus::accepted);
        std::filesystem::remove_all(directory);
        std::cout << "UI authentication checks passed\n";
    } catch (...) {
        std::filesystem::remove_all(directory);
        std::cerr << "UI authentication checks failed\n";
        return 1;
    }
}
