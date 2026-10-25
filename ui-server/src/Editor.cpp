#include "Editor.h"
#include <Wt/WCheckBox.h>
#include <Wt/WComboBox.h>
#include <Wt/WLabel.h>
#include <Wt/WLineEdit.h>
#include <Wt/WPasswordEdit.h>
#include <Wt/WPushButton.h>
#include <Wt/WText.h>
#include <Wt/WTextArea.h>
#include <charconv>
#include <sstream>

namespace mailadmin::ui {
namespace {
Wt::WString string(const std::string& value) { return Wt::WString::fromUTF8(value); }
void caption(Wt::WContainerWidget* parent, Wt::WFormWidget* input, const std::string& title) {
    auto* label = parent->addWidget(std::make_unique<Wt::WLabel>(string(title)));
    label->setBuddy(input);
    input->setStyleClass("edit-input");
}
}
Editor::Editor(std::string title, std::function<void(Object)> save, std::function<void()> cancel, bool deleting) {
    auto* heading = addWidget(std::make_unique<Wt::WText>(string(title), Wt::TextFormat::Plain));
    heading->setStyleClass("heading");
    fields_ = addWidget(std::make_unique<Wt::WContainerWidget>());
    error_ = addWidget(std::make_unique<Wt::WText>("", Wt::TextFormat::Plain));
    error_->setAttributeValue("role", "alert");
    auto* actions = addWidget(std::make_unique<Wt::WContainerWidget>());
    actions->setStyleClass("toolbar");
    save_ = actions->addWidget(std::make_unique<Wt::WPushButton>(deleting ? "Delete" : "Save"));
    cancel_ = actions->addWidget(std::make_unique<Wt::WPushButton>("Cancel"));
    save_->clicked().connect([this, save = std::move(save)] {
        try {
            Object result;
            for (const auto& [key, read] : values_) if (auto value = read()) result[key] = std::move(*value);
            error_->setText("");
            save(std::move(result));
        } catch (...) { error_->setText("Review the form: use whole numbers, valid JSON where requested, and a password of at least 12 characters."); }
    });
    cancel_->clicked().connect(std::move(cancel));
}
Wt::WContainerWidget* Editor::field(std::string) {
    auto* result = fields_->addWidget(std::make_unique<Wt::WContainerWidget>());
    result->setStyleClass("form-row");
    return result;
}
void Editor::explain(std::string message) {
    fields_->addWidget(std::make_unique<Wt::WText>(string(message), Wt::TextFormat::Plain));
}
void Editor::text_field(std::string key, std::string title, std::string value, int maximum, bool multiline) {
    auto* parent = field(title);
    if (multiline) {
        auto* input = parent->addWidget(std::make_unique<Wt::WTextArea>(string(value)));
        input->setRows(5); caption(parent, input, title);
        values_[key] = [input, maximum]() -> std::optional<Value> {
            auto value = input->text().toUTF8();
            if (value.size() > static_cast<std::size_t>(maximum)) throw std::runtime_error{"Too long"};
            return Value(std::move(value));
        };
    } else {
        auto* input = parent->addWidget(std::make_unique<Wt::WLineEdit>(string(value)));
        input->setMaxLength(maximum); caption(parent, input, title);
        values_[key] = [input]() -> std::optional<Value> { return Value(input->text().toUTF8()); };
    }
}
void Editor::choice(std::string key, std::string title, std::vector<std::pair<std::string, Value>> choices, Value value) {
    auto* parent = field(title);
    auto* input = parent->addWidget(std::make_unique<Wt::WComboBox>());
    for (std::size_t index = 0; index < choices.size(); ++index) {
        input->addItem(string(choices[index].first));
        if (choices[index].second == value) input->setCurrentIndex(static_cast<int>(index));
    }
    caption(parent, input, title);
    values_[key] = [input, choices = std::move(choices)]() -> std::optional<Value> {
        if (input->currentIndex() < 0) throw std::runtime_error{"Select an option"};
        return choices.at(input->currentIndex()).second;
    };
}
void Editor::boolean(std::string key, std::string title, bool value) {
    choice(std::move(key), std::move(title), {{"Yes", true}, {"No", false}}, value);
}
void Editor::inherited_text(std::string key, std::string title, Value value) {
    auto* parent = field(title);
    auto* inherit = parent->addWidget(std::make_unique<Wt::WCheckBox>("Inherit from organization"));
    inherit->setChecked(value.is_null());
    auto* input = parent->addWidget(std::make_unique<Wt::WLineEdit>(value.is_null() ? Wt::WString{} : string(text(value))));
    input->setMaxLength(64); input->setDisabled(inherit->isChecked()); caption(parent, input, title);
    inherit->changed().connect([inherit, input] { input->setDisabled(inherit->isChecked()); });
    values_[key] = [inherit, input]() -> std::optional<Value> { return inherit->isChecked() ? Value{} : Value(input->text().toUTF8()); };
}
void Editor::integer(std::string key, std::string title, Value value, bool inherited) {
    auto* parent = field(title);
    auto* input = parent->addWidget(std::make_unique<Wt::WLineEdit>(value.is_null() ? Wt::WString{} : string(text(value))));
    input->setMaxLength(10); caption(parent, input, title + (inherited ? " (blank inherits)" : ""));
    values_[key] = [input, inherited]() -> std::optional<Value> {
        const auto value = input->text().toUTF8();
        if (value.empty() && inherited) return Value{};
        std::int64_t result = 0;
        const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), result);
        if (error != std::errc{} || end != value.data() + value.size()) throw std::runtime_error{"Invalid integer"};
        return Value(result);
    };
}
void Editor::addresses(std::string key, std::string title, const boost::json::array& values) {
    std::string content;
    for (const auto& value : values) { if (!content.empty()) content += '\n'; content += text(value); }
    auto* parent = field(title);
    auto* input = parent->addWidget(std::make_unique<Wt::WTextArea>(string(content)));
    input->setRows(5); caption(parent, input, title + " (one per line)");
    values_[key] = [input]() -> std::optional<Value> {
        boost::json::array result;
        std::istringstream stream{input->text().toUTF8()};
        for (std::string line; std::getline(stream, line);) {
            const auto first = line.find_first_not_of(" \t\r");
            if (first == std::string::npos) continue;
            result.emplace_back(line.substr(first, line.find_last_not_of(" \t\r") - first + 1));
        }
        if (result.size() > 32) throw std::runtime_error{"Too many destinations"};
        return result;
    };
}
void Editor::configuration(std::string key, std::string title, const Object& value) {
    auto* parent = field(title);
    auto* input = parent->addWidget(std::make_unique<Wt::WTextArea>(string(boost::json::serialize(value))));
    input->setRows(14); caption(parent, input, title);
    values_[key] = [input]() -> std::optional<Value> {
        const auto content = input->text().toUTF8();
        if (content.size() > 15000) throw std::runtime_error{"Too long"};
        auto value = boost::json::parse(content);
        if (!value.is_object()) throw std::runtime_error{"JSON object required"};
        return value;
    };
}
void Editor::password(bool creating) {
    auto* parent = field("Password");
    auto* mode = parent->addWidget(std::make_unique<Wt::WComboBox>());
    mode->addItem("Keep existing password"); mode->addItem("Generate secure password"); mode->addItem("Supply password");
    mode->setCurrentIndex(creating ? 1 : 0); caption(parent, mode, "Password action");
    auto* input = parent->addWidget(std::make_unique<Wt::WPasswordEdit>());
    input->setMaxLength(1024); input->setAutoComplete(false); caption(parent, input, "New password");
    values_["generate_password"] = [mode, creating]() -> std::optional<Value> {
        if (creating && mode->currentIndex() == 0) throw std::runtime_error{"Password required"};
        if (mode->currentIndex() == 1) return Value(true);
        return std::nullopt;
    };
    values_["password"] = [mode, input]() -> std::optional<Value> {
        if (mode->currentIndex() != 2) return std::nullopt;
        auto value = input->text().toUTF8();
        if (value.size() < 12) throw std::runtime_error{"Password too short"};
        input->setText("");
        return Value(std::move(value));
    };
}
void Editor::mail_host() {
    choice("dns_mode", "Mail DNS provisioning", {{"Use configured shared mail hostname", "default"}, {"Provision a dedicated mail host", "dedicated"}, {"Skip DNS provisioning", "none"}}, "default");
    text_field("mail_hostname", "Dedicated mail hostname", "", 253);
    integer("mail_priority", "Dedicated MX priority", 10);
    addresses("mail_ipv4", "Dedicated host IPv4 addresses", {});
    addresses("mail_ipv6", "Dedicated host IPv6 addresses", {});
    explain("Shared hosting preserves existing host addresses. Fill the dedicated host fields only when provisioning a new mail host.");
}
void Editor::set_busy(bool value, bool uncertain) {
    fields_->setDisabled(value);
    save_->setDisabled(value || uncertain);
    cancel_->setDisabled(value);
}
}
