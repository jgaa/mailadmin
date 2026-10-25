#pragma once
#include <filesystem>
#include <memory>
#include <string>

namespace mailadmin::ui {
class Credential {
public:
    explicit Credential(std::string value);
    ~Credential();
    Credential(const Credential&) = delete;
    Credential& operator=(const Credential&) = delete;
    const std::string& value() const { return value_; }
private:
    std::string value_;
};

// CLI file path overrides MAILADMIN_API_KEY_FILE. A literal MAILADMIN_API_KEY
// and a selected file are mutually exclusive. No secret is a CLI argument.
std::shared_ptr<const Credential> load_credential(const std::filesystem::path& file = {});
}
