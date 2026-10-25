#pragma once

#include "ApiClient.h"
#include "Authentication.h"
#include <Wt/WApplication.h>
#include <map>

namespace Wt {
class WContainerWidget;
class WLineEdit;
class WPushButton;
class WPasswordEdit;
class WText;
class WTree;
class WTreeNode;
class WTimer;
}

namespace mailadmin::ui {
class Editor;
struct Selection { std::string resource; std::string id; };

class Application final : public Wt::WApplication {
public:
    Application(const Wt::WEnvironment& environment, std::shared_ptr<ApiClient> client, std::shared_ptr<const Credential> credential, std::shared_ptr<Authentication> authentication);
private:
    bool ensure_authenticated();
    void sign_in();
    void sign_out();
    void clear_data();
    void refresh();
    void select_mailbox();
    void load_mailbox(std::int64_t id);
    void check_result();
    void busy();
    void populate(const Inventory& inventory);
    void display(const Mailbox& mailbox);
    void load(Selection selection);
    void display_document(const Document& document);
    void render_selected();
    void edit(std::string resource, Object record = {});
    void remove(std::string resource, std::string id, std::string name);
    void submit(std::string method, std::string resource, std::string id, Object input);
    void mutation_finished(Mutation mutation);
    void policies(Editor& editor, const Object& record, bool inherited);

    std::shared_ptr<ApiClient> client_;
    std::shared_ptr<const Credential> credential_;
    std::shared_ptr<Pending> pending_;
    std::map<Wt::WTreeNode*, std::int64_t> mailboxes_;
    std::int64_t selected_id_ = 0;
    Selection selected_;
    std::map<Wt::WTreeNode*, Selection> selections_;
    Inventory inventory_;
    std::optional<Mailbox> mailbox_;
    std::optional<Document> document_;
    std::optional<Selection> reload_selection_;
    Editor* editor_ = nullptr;
    bool uncertain_ = false;
    bool api_authorized_ = false;
    bool authenticated_ = false;
    std::string login_revision_;
    std::shared_ptr<Authentication> authentication_;
    Wt::WContainerWidget* login_;
    Wt::WContainerWidget* administration_;
    Wt::WLineEdit* username_;
    Wt::WPasswordEdit* password_;
    Wt::WPushButton* new_organization_;
    Wt::WPushButton* operations_;
    Wt::WContainerWidget* notice_;
    Wt::WPushButton* refresh_;
    Wt::WPushButton* refresh_mailbox_;
    Wt::WPushButton* check_;
    Wt::WText* status_;
    Wt::WTree* tree_;
    Wt::WContainerWidget* details_;
    Wt::WTimer* timer_;
};

}
