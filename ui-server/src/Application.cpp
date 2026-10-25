#include "Application.h"
#include "logging.h"
#include "Editor.h"

#include <Wt/WContainerWidget.h>
#include <Wt/WEnvironment.h>
#include <Wt/WLabel.h>
#include <Wt/WLineEdit.h>
#include <Wt/WPasswordEdit.h>
#include <openssl/crypto.h>
#include <Wt/WCssStyleSheet.h>
#include <Wt/WPushButton.h>
#include <Wt/WTable.h>
#include <Wt/WText.h>
#include <Wt/WTimer.h>
#include <Wt/WTree.h>
#include <Wt/WTreeNode.h>
#include <chrono>
#include <algorithm>

namespace mailadmin::ui {
namespace {
Wt::WText* label(Wt::WContainerWidget* parent, const std::string& value, const std::string& style = {}) {
    auto* widget = parent->addWidget(std::make_unique<Wt::WText>(Wt::WString::fromUTF8(value), Wt::TextFormat::Plain));
    if (!style.empty()) widget->setStyleClass(style);
    return widget;
}
void row(Wt::WTable* table, const std::string& name, const std::string& value) {
    const auto index = table->rowCount();
    label(table->elementAt(index, 0), name, "field-name");
    label(table->elementAt(index, 1), value, "field-value");
}
Wt::WTreeNode* node(Wt::WTreeNode* parent, const std::string& name) {
    auto* result = parent->addChildNode(std::make_unique<Wt::WTreeNode>(""));
    result->label()->setTextFormat(Wt::TextFormat::Plain);
    result->label()->setText(Wt::WString::fromUTF8(name));
    return result;
}
std::string strings(const boost::json::array& values) {
    std::string result;
    for (const auto& value : values) {
        if (!result.empty()) result += '\n';
        result += text(value);
    }
    return result.empty() ? "None" : result;
}
}

Application::Application(const Wt::WEnvironment& environment, std::shared_ptr<ApiClient> client, std::shared_ptr<const Credential> credential, std::shared_ptr<Authentication> authentication)
    : WApplication{environment}, client_{std::move(client)}, credential_{std::move(credential)}, authentication_{std::move(authentication)} {
    setTitle("Mailadmin — administration");
    styleSheet().addRule("body", "margin:0;background:#f4f6f8;color:#172638;font:15px system-ui,sans-serif;");
    styleSheet().addRule(".page", "max-width:1300px;margin:auto;padding:24px;");
    styleSheet().addRule(".heading", "display:block;font-size:26px;font-weight:650;margin-bottom:8px;");
    styleSheet().addRule(".subtitle", "display:block;color:#526476;margin-bottom:20px;");
    styleSheet().addRule(".toolbar", "display:flex;gap:10px;flex-wrap:wrap;margin:16px 0;align-items:center;");
    styleSheet().addRule("button", "padding:8px 12px;border:1px solid #a5b2c0;border-radius:5px;background:white;cursor:pointer;");
    styleSheet().addRule("button:disabled", "opacity:.5;cursor:default;");
    styleSheet().addRule("input", "padding:8px;border:1px solid #a5b2c0;border-radius:5px;");
    styleSheet().addRule(".workspace", "display:flex;gap:20px;flex-wrap:wrap;align-items:flex-start;");
    styleSheet().addRule(".panel", "padding:20px;background:white;border:1px solid #d9e0e7;border-radius:8px;min-width:260px;");
    styleSheet().addRule(".login-panel", "width:100%;max-width:400px;min-width:0;box-sizing:border-box;");
    styleSheet().addRule(".login-panel .form-row", "margin:0 0 16px;");
    styleSheet().addRule(".login-panel button", "width:100%;margin-top:4px;");
    styleSheet().addRule(".navigation", "flex:1;overflow:auto;");
    styleSheet().addRule(".details", "flex:2;overflow-wrap:anywhere;");
    styleSheet().addRule(".section", "display:block;font-weight:650;font-size:18px;margin:16px 0 10px;");
    styleSheet().addRule(".status", "display:block;min-height:24px;margin:12px 0;color:#314e70;");
    styleSheet().addRule("td", "padding:7px 12px 7px 0;vertical-align:top;");
    styleSheet().addRule(".field-name", "font-weight:600;");
    styleSheet().addRule(".field-value", "white-space:pre-wrap;");
    styleSheet().addRule(".policy", "border-collapse:collapse;width:100%;");
    styleSheet().addRule(".form-row", "display:flex;flex-direction:column;gap:6px;margin:12px 0;");
    styleSheet().addRule(".form-row label", "order:-1;font-weight:600;");
    styleSheet().addRule(".edit-input", "width:100%;box-sizing:border-box;padding:8px;");
    styleSheet().addRule(".notice", "padding:12px;background:#eaf2fa;white-space:pre-wrap;margin:12px 0;");
    auto* page = root()->addWidget(std::make_unique<Wt::WContainerWidget>());
    page->setStyleClass("page");
    label(page, "Mailadmin", "heading");
    label(page, "Mail server administration", "subtitle");
    login_ = page->addWidget(std::make_unique<Wt::WContainerWidget>());
    login_->setStyleClass("panel login-panel");
    auto* username_row = login_->addWidget(std::make_unique<Wt::WContainerWidget>());
    username_row->setStyleClass("form-row");
    auto* username_label = username_row->addWidget(std::make_unique<Wt::WLabel>("Username"));
    username_ = username_row->addWidget(std::make_unique<Wt::WLineEdit>());
    username_label->setBuddy(username_);
    username_->setStyleClass("edit-input");
    username_->setText(Wt::WString::fromUTF8(authentication_->username()));
    username_->setAttributeValue("autocomplete", "username");
    auto* password_row = login_->addWidget(std::make_unique<Wt::WContainerWidget>());
    password_row->setStyleClass("form-row");
    auto* password_label = password_row->addWidget(std::make_unique<Wt::WLabel>("Password"));
    password_ = password_row->addWidget(std::make_unique<Wt::WPasswordEdit>());
    password_label->setBuddy(password_);
    password_->setStyleClass("edit-input");
    password_->setAttributeValue("autocomplete", "current-password");
    login_->addWidget(std::make_unique<Wt::WPushButton>("Sign in"))->clicked().connect(this, &Application::sign_in);
    password_->enterPressed().connect(this, &Application::sign_in);
    status_ = label(page, "Sign in to administer the mail server.", "status");
    status_->setAttributeValue("role", "status");
    administration_ = page->addWidget(std::make_unique<Wt::WContainerWidget>());
    administration_->hide();
    auto* toolbar = administration_->addWidget(std::make_unique<Wt::WContainerWidget>());
    toolbar->setStyleClass("toolbar");
    new_organization_ = toolbar->addWidget(std::make_unique<Wt::WPushButton>("New organization"));
    new_organization_->clicked().connect([this] { edit("organizations"); });
    operations_ = toolbar->addWidget(std::make_unique<Wt::WPushButton>("Operations"));
    operations_->clicked().connect([this] { load({"operations", ""}); });
    refresh_ = toolbar->addWidget(std::make_unique<Wt::WPushButton>("Refresh tree"));
    refresh_->clicked().connect(this, &Application::refresh);
    refresh_mailbox_ = toolbar->addWidget(std::make_unique<Wt::WPushButton>("Refresh mailbox"));
    refresh_mailbox_->clicked().connect([this] { load_mailbox(selected_id_); });
    check_ = toolbar->addWidget(std::make_unique<Wt::WPushButton>("Check for results"));
    check_->clicked().connect(this, &Application::check_result);
    toolbar->addWidget(std::make_unique<Wt::WPushButton>("Sign out"))->clicked().connect(this, &Application::sign_out);
    label(administration_, "Without JavaScript, use Check for results after starting a request.", "subtitle");
    notice_ = administration_->addWidget(std::make_unique<Wt::WContainerWidget>());
    auto* workspace = administration_->addWidget(std::make_unique<Wt::WContainerWidget>());
    workspace->setStyleClass("workspace");
    auto* navigation = workspace->addWidget(std::make_unique<Wt::WContainerWidget>());
    navigation->setStyleClass("panel navigation");
    label(navigation, "Organizations / domains / email", "section");
    tree_ = navigation->addWidget(std::make_unique<Wt::WTree>());
    tree_->setSelectionMode(Wt::SelectionMode::Single);
    tree_->itemSelectionChanged().connect(this, &Application::select_mailbox);
    details_ = workspace->addWidget(std::make_unique<Wt::WContainerWidget>());
    details_->setStyleClass("panel details");
    label(details_, "Select a mailbox to view its configuration.");

    // WTimer is an Ajax convenience only. Plain HTML uses the same completion
    // handler through an ordinary form button; no push/JavaScript is required.
    auto timer = std::make_unique<Wt::WTimer>();
    timer->setInterval(std::chrono::seconds{1});
    timer->timeout().connect(this, &Application::check_result);
    timer_ = timer.get();
    addChild(std::move(timer));
    busy();
    UI_DEBUG << "Administration UI session created";
}

bool Application::ensure_authenticated() {
    if (!authenticated_) return false;
    if (authentication_->current(login_revision_)) return true;
    sign_out();
    status_->setText("Administrator password changed. Sign in again.");
    return false;
}
void Application::sign_in() {
    if (authenticated_) return;
    auto password = password_->text().toUTF8();
    password_->setText("");
    LoginResult result{LoginStatus::rejected,{}};
    try { result = authentication_->verify(username_->text().toUTF8(), password, environment().clientAddress()); }
    catch (...) { UI_ERROR << "UI authentication storage could not be read"; }
    OPENSSL_cleanse(password.data(),password.size());
    if (result.status != LoginStatus::accepted) {
        status_->setText(result.status == LoginStatus::rate_limited
            ? "Too many sign-in attempts. Try again in one minute." : "Username or password incorrect.");
        UI_DEBUG << "UI sign-in rejected";
        return;
    }
    changeSessionId();
    authenticated_ = true;
    login_revision_ = std::move(result.revision);
    login_->hide(); administration_->show();
    UI_DEBUG << "UI administrator signed in";
    refresh();
}
void Application::sign_out() {
    authenticated_ = false; login_revision_.clear(); pending_.reset();
    clear_data(); busy();
    login_->show(); administration_->hide(); password_->setText("");
    changeSessionId();
    status_->setText("Signed out.");
    UI_DEBUG << "UI administrator signed out";
}
void Application::busy() {
    const bool loading = static_cast<bool>(pending_);
    const bool connected = authenticated_ && api_authorized_;
    refresh_->setDisabled(loading || editor_);
    new_organization_->setDisabled(loading || !connected || editor_);
    operations_->setDisabled(loading || !connected || editor_);
    refresh_mailbox_->setDisabled(loading || !connected || selected_id_ == 0 || editor_);
    tree_->setDisabled(loading || !connected || editor_);
    if (editor_) editor_->set_busy(loading, uncertain_);
    check_->setDisabled(!loading);
    // Poll only during a transfer; an idle page must not keep its credential
    // alive indefinitely by making timer requests every second.
    if (loading) timer_->start();
    else timer_->stop();
}
void Application::clear_data() {
    api_authorized_ = false;
    selected_id_ = 0;
    mailboxes_.clear(); selections_.clear(); selected_ = {}; inventory_ = {};
    mailbox_.reset(); document_.reset(); reload_selection_.reset(); editor_ = nullptr;
    notice_->clear();
    tree_->setTreeRoot(std::make_unique<Wt::WTreeNode>("API unavailable"));
    details_->clear();
}
void Application::refresh() {
    if (!ensure_authenticated() || !credential_ || pending_ || editor_) return;
    selected_id_ = 0;
    selected_ = {}; mailbox_.reset(); document_.reset(); selections_.clear();
    mailboxes_.clear();
    tree_->setTreeRoot(std::make_unique<Wt::WTreeNode>("Loading…"));
    details_->clear();
    status_->setText("Loading organizations, domains and mailboxes…");
    pending_ = client_->inventory(credential_);
    busy();
}
void Application::select_mailbox() {
    if (!ensure_authenticated() || pending_ || editor_ || tree_->selectedNodes().empty()) return;
    const auto found = selections_.find(*tree_->selectedNodes().begin());
    if (found != selections_.end()) load(found->second);
}
void Application::load(Selection selection) {
    if (!ensure_authenticated() || !credential_ || pending_ || editor_) return;
    selected_ = std::move(selection);
    selected_id_ = selected_.resource == "accounts" ? std::stoll(selected_.id) : 0;
    details_->clear();
    status_->setText("Loading current configuration…");
    if (selected_id_) pending_ = client_->mailbox(credential_, selected_id_);
    else pending_ = client_->document(credential_, selected_.resource, selected_.id);
    busy();
}
void Application::load_mailbox(std::int64_t id) {
    if (id != 0) load({"accounts", std::to_string(id)});
}
void Application::check_result() {
    if (!pending_ || !ensure_authenticated()) return;
    std::optional<Outcome> result;
    {
        std::lock_guard lock{pending_->mutex};
        if (!pending_->outcome) return;
        result = std::move(pending_->outcome);
    }
    pending_.reset();
    if (!result->error.empty()) {
        if (result->unauthorized) clear_data();
        uncertain_ = result->uncertain;
        status_->setText(Wt::WString::fromUTF8(result->error));
    } else if (result->payload) {
        api_authorized_ = true;
        if (const auto* inventory = std::get_if<Inventory>(&*result->payload)) populate(*inventory);
        else if (auto* mailbox = std::get_if<Mailbox>(&*result->payload)) {
            mailbox_ = std::move(*mailbox); document_.reset(); display(*mailbox_);
        } else if (auto* document = std::get_if<Document>(&*result->payload)) {
            document_ = std::move(*document); mailbox_.reset(); display_document(*document_);
        } else mutation_finished(std::move(std::get<Mutation>(*result->payload)));
    }
    busy();
}
void Application::populate(const Inventory& inventory) {
    inventory_ = inventory;
    mailboxes_.clear(); selections_.clear();
    auto root = std::make_unique<Wt::WTreeNode>("Mail server");
    root->label()->setTextFormat(Wt::TextFormat::Plain);
    std::map<std::int64_t, Wt::WTreeNode*> organizations;
    std::map<std::string, Wt::WTreeNode*> domains;
    for (const auto& organization : inventory.organizations)
    {
        auto* item = node(root.get(), organization.name);
        organizations.emplace(organization.id, item);
        selections_.emplace(item, Selection{"organizations", std::to_string(organization.id)});
    }
    for (const auto& domain : inventory.domains)
    {
        auto* item = node(organizations.at(domain.organization_id), domain.name);
        domains.emplace(domain.name, item);
        selections_.emplace(item, Selection{"domains", domain.name});
    }
    for (const auto& account : inventory.accounts)
    {
        auto* item = node(domains.at(address_domain(account.address)), account.address);
        mailboxes_.emplace(item, account.id);
        selections_.emplace(item, Selection{"accounts", std::to_string(account.id)});
    }
    root->expand();
    for (const auto& [id, organization] : organizations) organization->expand();
    for (const auto& [name, domain] : domains) domain->expand();
    tree_->setTreeRoot(std::move(root));
    label(details_, "Select a mailbox to view its current settings and aliases.");
    status_->setText(Wt::WString::fromUTF8("Loaded " + std::to_string(inventory.organizations.size()) + " organizations, "
        + std::to_string(inventory.domains.size()) + " domains and " + std::to_string(inventory.accounts.size()) + " mailboxes."));
    UI_DEBUG << "UI inventory displayed; accounts=" << inventory.accounts.size();
    if (reload_selection_) {
        auto selection = std::move(*reload_selection_); reload_selection_.reset();
        if (std::ranges::any_of(selections_, [&](const auto& item) { return item.second.resource == selection.resource && item.second.id == selection.id; })) load(std::move(selection));
    }
}
void Application::display(const Mailbox& mailbox) {
    details_->clear();
    auto* actions = details_->addWidget(std::make_unique<Wt::WContainerWidget>());
    actions->setStyleClass("toolbar");
    actions->addWidget(std::make_unique<Wt::WPushButton>("Edit mailbox"))->clicked().connect([this] { edit("accounts", mailbox_->account); });
    actions->addWidget(std::make_unique<Wt::WPushButton>("Edit forwarding"))->clicked().connect([this] { edit("forwarding", mailbox_->account); });
    actions->addWidget(std::make_unique<Wt::WPushButton>("Add alias"))->clicked().connect([this] { edit("aliases"); });
    actions->addWidget(std::make_unique<Wt::WPushButton>("Delete mailbox"))->clicked().connect([this] { remove("accounts", selected_.id, text(mailbox_->account.at("address"))); });
    const auto& account = mailbox.account;
    const auto& organization = mailbox.organization;
    label(details_, text(account.at("address")), "heading");
    auto* table = details_->addWidget(std::make_unique<Wt::WTable>());
    row(table, "Account ID", text(account.at("id")));
    row(table, "Name", text(account.at("name")));
    row(table, "Organization", text(organization.at("name")));
    row(table, "Domain", address_domain(text(account.at("address"))));
    row(table, "Organization hosted", text(organization.at("hosted")));
    row(table, "Login currently enabled", text(account.at("active")));
    row(table, "Desired enabled state", text(account.at("desired_active")));
    row(table, "Quota (Courier)", text(account.at("quota")).empty() ? "Unlimited" : text(account.at("quota")));
    row(table, "Forwarding destinations", strings(account.at("forwards").as_array()));
    row(table, "Keep local copy when forwarding", text(account.at("keep_copy")));
    row(table, "Auto-reply", text(account.at("auto_reply")).empty() ? "Disabled" : text(account.at("auto_reply")));
    row(table, "Inbound mail server", text(organization.at("inbound_mailserver")));
    row(table, "Outbound mail server", text(organization.at("outbound_mailserver")));

    label(details_, "Delivery and spam policies", "section");
    auto* policies = details_->addWidget(std::make_unique<Wt::WTable>());
    policies->setStyleClass("policy");
    policies->setHeaderCount(1);
    label(policies->elementAt(0, 0), "Policy");
    label(policies->elementAt(0, 1), "Account setting");
    label(policies->elementAt(0, 2), "Effective setting");
    for (const auto& [field, name] : std::initializer_list<std::pair<const char*, const char*>>{
            {"filter_enabled", "Managed delivery filters"}, {"spam_enabled", "Spam handling"},
            {"spam_folder", "Spam folder"}, {"spam_retention_days", "Retention (days)"}, {"spam_trash", "Expired spam destination"}}) {
        const auto& setting = account.at(field);
        const auto& effective = setting.is_null() ? organization.at(field) : setting;
        const auto index = policies->rowCount();
        label(policies->elementAt(index, 0), name, "field-name");
        label(policies->elementAt(index, 1), text(setting));
        label(policies->elementAt(index, 2), std::string{field} == "spam_trash" && text(effective).empty()
            ? "Delete expired spam" : text(effective));
    }
    label(details_, "Aliases", "section");
    if (mailbox.alias_records.empty()) label(details_, "No aliases.");
    else {
        auto* aliases = details_->addWidget(std::make_unique<Wt::WTable>());
        for (const auto& value : mailbox.alias_records) {
            const auto record = value.as_object();
            const auto index = aliases->rowCount();
            label(aliases->elementAt(index, 0), text(record.at("address")));
            aliases->elementAt(index, 1)->addWidget(std::make_unique<Wt::WPushButton>("Edit alias"))->clicked().connect([this, record] { edit("aliases", record); });
            aliases->elementAt(index, 2)->addWidget(std::make_unique<Wt::WPushButton>("Delete alias"))->clicked().connect([this, record] { remove("aliases", std::to_string(record_id(record.at("id"))), text(record.at("address"))); });
        }
    }
    status_->setText("Current API configuration loaded. Refresh mailbox to fetch it again.");
    UI_DEBUG << "Mailbox details displayed; account_id=" << selected_id_ << " aliases=" << mailbox.aliases.size();
}

}
