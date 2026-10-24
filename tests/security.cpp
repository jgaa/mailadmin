#include "Security.h"
#include <filesystem>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>

namespace {
void require(bool condition) {
    if (!condition) throw std::runtime_error{"Security regression failed"};
}
template<typename Function> void rejects(Function function) {
    bool rejected = false;
    try { function(); } catch (const std::exception&) { rejected = true; }
    require(rejected);
}
std::string capture_key(const std::filesystem::path& path, bool rotate) {
    std::ostringstream output;
    auto* previous = std::cout.rdbuf(output.rdbuf());
    try { mailadmin::create_key(path, rotate); }
    catch (...) { std::cout.rdbuf(previous); throw; }
    std::cout.rdbuf(previous);
    auto token = output.str();
    require(token.ends_with('\n'));
    token.pop_back();
    return token;
}
}
int main() {
    char directory[] = "/tmp/mailadmin-security-XXXXXX";
    if (!mkdtemp(directory)) return 1;
    struct Cleanup {
        std::filesystem::path path;
        ~Cleanup() { std::filesystem::remove_all(path); }
    } cleanup{directory};
    try {
        using namespace mailadmin;
        const auto path = cleanup.path / "key.sha256";
        const auto first = capture_key(path, false);
        const auto verifier = load_key(path);
        require(first.starts_with("ma_") && first.size() == 67);
        require(verify_key("Bearer " + first, verifier));
        require(!verify_key(first, verifier));
        require(!verify_key("Basic " + first, verifier));
        require(!verify_key("Bearer " + first + "extra", verifier));
        require(hex(verifier) != first.substr(3));
        rejects([&] { create_key(path, false); });
        const auto second = capture_key(path, true);
        require(second != first);
        const auto replacement = load_key(path);
        require(verify_key("Bearer " + second, replacement));
        require(!verify_key("Bearer " + first, replacement));
        const auto symlink = cleanup.path / "symlink";
        std::filesystem::create_symlink(path, symlink);
        rejects([&] { load_key(symlink); });
        require(chmod(path.c_str(), 0644) == 0);
        rejects([&] { load_key(path); });
        require(chmod(path.c_str(), 0600) == 0);
        require(chmod(directory, 0770) == 0);
        rejects([&] { create_key(cleanup.path / "unsafe-key", false); });
        require(chmod(directory, 0700) == 0);
        std::cout << "Security regression checks passed\n";
        return 0;
    } catch (const std::exception&) {
        std::cerr << "Security regression checks failed\n";
        return 1;
    }
}
