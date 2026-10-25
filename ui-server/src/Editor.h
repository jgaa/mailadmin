#pragma once
#include "Data.h"
#include <Wt/WContainerWidget.h>
#include <functional>
#include <map>
#include <optional>

namespace Wt { class WPushButton; class WText; }
namespace mailadmin::ui {
class Editor final : public Wt::WContainerWidget {
public:
    Editor(std::string title, std::function<void(Object)> save, std::function<void()> cancel, bool deleting = false);
    void text_field(std::string key, std::string title, std::string value, int maximum = 255, bool multiline = false);
    void choice(std::string key, std::string title, std::vector<std::pair<std::string, Value>> choices, Value value);
    void boolean(std::string key, std::string title, bool value);
    void inherited_text(std::string key, std::string title, Value value);
    void integer(std::string key, std::string title, Value value, bool inherited = false);
    void addresses(std::string key, std::string title, const boost::json::array& values);
    void configuration(std::string key, std::string title, const Object& value);
    void password(bool creating);
    void mail_host();
    void explain(std::string message);
    void set_busy(bool value, bool uncertain = false);
private:
    Wt::WContainerWidget* field(std::string title);
    Wt::WContainerWidget* fields_;
    Wt::WText* error_;
    Wt::WPushButton* save_;
    Wt::WPushButton* cancel_;
    std::map<std::string, std::function<std::optional<Value>()>> values_;
};
}
