#include "Application.h"
#include "Editor.h"
#include <Wt/WContainerWidget.h>
#include <Wt/WPushButton.h>
#include <Wt/WTable.h>
#include <Wt/WText.h>
#include <algorithm>

namespace mailadmin::ui {
namespace {
void label(Wt::WContainerWidget* parent, const std::string& value, std::string style = {}) {
    auto* widget = parent->addWidget(std::make_unique<Wt::WText>(Wt::WString::fromUTF8(value), Wt::TextFormat::Plain));
    if (!style.empty()) widget->setStyleClass(style);
}
Value field(const Object& record, std::string_view name, Value fallback = {}) {
    if (const auto* value = record.if_contains(name)) return *value;
    return fallback;
}
std::string string_field(const Object& record, std::string_view name, std::string fallback = {}) {
    if (const auto* value = record.if_contains(name); value && value->is_string()) return std::string{value->as_string()};
    return fallback;
}
}
void Application::policies(Editor& editor, const Object& record, bool inherited) {
    for (const auto& [key, title] : std::initializer_list<std::pair<const char*, const char*>>{
            {"filter_enabled", "Managed filters"}, {"spam_enabled", "Spam cleanup"}}) {
        const auto fallback = inherited ? Value{} : Value(std::string{key} == "filter_enabled");
        if (inherited) editor.choice(key, title, {{"Inherit from organization", {}}, {"Enabled", true}, {"Disabled", false}}, field(record, key, fallback));
        else editor.boolean(key, title, field(record, key, fallback).as_bool());
    }
    for (const auto& [key, title, fallback] : std::initializer_list<std::tuple<const char*, const char*, const char*>>{
            {"spam_folder", "Spam folder", "spam"}, {"spam_trash", "Expired spam destination (empty deletes)", "Trash"}}) {
        const auto value = field(record, key, inherited ? Value{} : Value(fallback));
        if (inherited) editor.inherited_text(key, title, value);
        else editor.text_field(key, title, text(value), 64);
    }
    editor.integer("spam_retention_days", "Spam retention days", field(record, "spam_retention_days", inherited ? Value{} : Value(30)), inherited);
}

void Application::render_selected() {
    const bool reload = uncertain_;
    editor_ = nullptr; uncertain_ = false;
    details_->clear();
    if (reload) {
        if (selected_.id.empty()) refresh();
        else load(selected_);
        return;
    }
    if (mailbox_) display(*mailbox_);
    else if (document_) display_document(*document_);
    else label(details_, "Select an organization, domain or mailbox.");
    busy();
}
void Application::display_document(const Document& document) {
    details_->clear();
    const auto& output = document.value;
    if (document.resource == "operations") {
        label(details_, "Background operations", "heading");
        if (const auto* activation = output.if_contains("activation")) label(details_, "Courier: " + boost::json::serialize(*activation), "field-value");
        if (const auto* cleanup = output.if_contains("cleanup")) label(details_, "Mailbox cleanup: " + boost::json::serialize(*cleanup), "field-value");
        label(details_, "Mail DNS", "section");
        auto* table = details_->addWidget(std::make_unique<Wt::WTable>());
        if (const auto* jobs = output.if_contains("dns"); jobs && jobs->is_array()) {
            for (const auto& job : jobs->as_array()) {
                const auto index = table->rowCount();
                label(table->elementAt(index, 0), text(job.at("domain")));
                label(table->elementAt(index, 1), text(job.at("status")));
                label(table->elementAt(index, 2), text(job.at("error")));
                table->elementAt(index, 3)->addWidget(std::make_unique<Wt::WPushButton>("DNS details"))->clicked().connect([this, domain = text(job.at("domain"))] { load({"dns", domain}); });
            }
        }
        details_->addWidget(std::make_unique<Wt::WPushButton>("Refresh operations"))->clicked().connect([this] { load({"operations", ""}); });
    } else if (document.resource == "dns") {
        const auto& dns = output.at("dns").as_object();
        label(details_, "DNS for " + selected_.id, "heading");
        label(details_, "Status: " + text(dns.at("status")));
        if (const auto* managed = dns.if_contains("managed")) label(details_, "DNS ownership confirmed: " + text(*managed));
        if (const auto* error = dns.if_contains("error"); error && !text(*error).empty()) label(details_, "Error: " + text(*error));
        if (const auto* deadline = dns.if_contains("earliest_completion"); deadline && !deadline->is_null())
            label(details_, "Earliest safe completion (UTC Unix seconds): " + text(*deadline));
        if (const auto* report = dns.if_contains("report"); report && !report->is_null())
            label(details_, boost::json::serialize(*report), "field-value");
        auto* actions = details_->addWidget(std::make_unique<Wt::WContainerWidget>());
        actions->setStyleClass("toolbar");
        if (dns.at("enabled").as_bool()) {
            actions->addWidget(std::make_unique<Wt::WPushButton>("Configure DNS"))->clicked().connect([this, dns] { edit("dns", dns); });
            if (dns.contains("desired")) actions->addWidget(std::make_unique<Wt::WPushButton>("Remove mail DNS"))->clicked().connect([this] { remove("dns", selected_.id, "mail DNS for " + selected_.id); });
        }
        actions->addWidget(std::make_unique<Wt::WPushButton>("Refresh DNS status"))->clicked().connect([this] { load({"dns", selected_.id}); });
        if (std::ranges::any_of(inventory_.domains, [this](const auto& domain) { return domain.name == selected_.id; }))
            actions->addWidget(std::make_unique<Wt::WPushButton>("Back to domain"))->clicked().connect([this] { load({"domains", selected_.id}); });
    } else {
        const bool organization = document.resource == "organizations";
        const auto& record = output.at(organization ? "organization" : "domain").as_object();
        label(details_, organization ? text(record.at("name")) : text(record.at("domain")), "heading");
        auto* table = details_->addWidget(std::make_unique<Wt::WTable>());
        for (const auto& member : record) {
            if (member.value().is_object() || member.value().is_array()) continue;
            const auto index = table->rowCount();
            label(table->elementAt(index, 0), std::string{member.key()}, "field-name");
            label(table->elementAt(index, 1), text(member.value()));
        }
        auto* actions = details_->addWidget(std::make_unique<Wt::WContainerWidget>());
        actions->setStyleClass("toolbar");
        actions->addWidget(std::make_unique<Wt::WPushButton>(organization ? "Edit organization" : "Edit domain"))->clicked().connect([this, record] { edit(selected_.resource, record); });
        actions->addWidget(std::make_unique<Wt::WPushButton>(organization ? "New domain" : "New mailbox"))->clicked().connect([this, organization] { edit(organization ? "domains" : "accounts"); });
        actions->addWidget(std::make_unique<Wt::WPushButton>(organization ? "Delete organization" : "Delete domain"))->clicked().connect([this, record, organization] { remove(selected_.resource, selected_.id, text(record.at(organization ? "name" : "domain"))); });
        if (!organization) actions->addWidget(std::make_unique<Wt::WPushButton>("DNS settings"))->clicked().connect([this] { load({"dns", selected_.id}); });
    }
    status_->setText("Current configuration loaded.");
}

void Application::edit(std::string resource, Object record) {
    if (!ensure_authenticated() || !credential_ || pending_ || editor_) return;
    notice_->clear(); uncertain_ = false;
    const bool creating = record.empty() && resource != "forwarding";
    const auto id = creating ? std::string{} : resource == "domains" || resource == "dns" ? selected_.id : std::to_string(record_id(record.at("id")));
    details_->clear();
    auto editor = std::make_unique<Editor>((creating ? "New " : "Edit ") + resource,
        [this, resource, id](Object input) {
            auto method = id.empty() ? "POST" : "PATCH";
            auto target = resource == "forwarding" ? std::string{"accounts"} : resource;
            auto record_id = id;
            if (input.contains("dns_mode")) {
                const auto mode = text(input.at("dns_mode"));
                if (mode == "none") input["provision_dns"] = false;
                else if (mode == "dedicated") {
                    Object host{{"hostname",input.at("mail_hostname")},{"priority",input.at("mail_priority")}};
                    if (!input.at("mail_ipv4").as_array().empty()) host["ipv4"] = input.at("mail_ipv4");
                    if (!input.at("mail_ipv6").as_array().empty()) host["ipv6"] = input.at("mail_ipv6");
                    input["mail_server"] = std::move(host);
                }
                for (const auto* key : {"dns_mode","mail_hostname","mail_priority","mail_ipv4","mail_ipv6"}) input.erase(key);
            }
            if (resource == "dns") {
                method = input.at("replace").as_bool() ? "PUT" : "POST";
                input.erase("replace"); record_id = selected_.id;
                input = input.at("configuration").as_object();
            }
            submit(method, target, record_id, std::move(input));
        }, [this] { render_selected(); });
    editor_ = details_->addWidget(std::move(editor));
    auto& form = *editor_;
    if (resource == "organizations") {
        form.text_field("name", "Organization name", string_field(record, "name"), 50);
        form.text_field("default_domain", "Default domain", string_field(record, "default_domain"), 253);
        form.boolean("hosted", "Hosted here", field(record, "hosted", true).as_bool());
        form.text_field("inbound_mailserver", "Inbound mail server", string_field(record, "inbound_mailserver"), 128);
        form.text_field("outbound_mailserver", "Outbound mail server", string_field(record, "outbound_mailserver"), 128);
        policies(form, record, false);
        if (creating) form.mail_host();
        if (!creating) form.explain("The default domain must already belong to this organization.");
    } else if (resource == "domains") {
        if (creating) form.text_field("domain", "Domain name", "", 253);
        if (creating) form.mail_host();
        std::vector<std::pair<std::string, Value>> choices;
        for (const auto& organization : inventory_.organizations) choices.emplace_back(organization.name, organization.id);
        Value owner = field(record, "organization_id");
        if (owner.is_null() && selected_.resource == "organizations") owner = std::stoll(selected_.id);
        form.choice("organization_id", "Owning organization", std::move(choices), owner);
        if (!creating) form.explain("Only an empty domain that is not an organization's default can change owner.");
    } else if (resource == "accounts" || resource == "forwarding") {
        if (resource == "accounts") {
            form.text_field("address", "Email address", string_field(record, "address", selected_.resource == "domains" ? "@" + selected_.id : ""), 128);
            form.text_field("name", "Display name", string_field(record, "name"), 128);
            form.text_field("quota", "Quota (bytes S, messages C; empty means unlimited)", string_field(record, "quota"), 255);
            form.boolean("active", "Enabled", field(record, "desired_active", true).as_bool());
            form.text_field("auto_reply", "Auto-reply (empty disables)", string_field(record, "auto_reply"), 8192, true);
            form.password(creating);
            policies(form, record, true);
        }
        form.addresses("forwards", "Forwarding destinations", field(record, "forwards", boost::json::array{}).as_array());
        form.boolean("keep_copy", "Keep a local copy when forwarding", field(record, "keep_copy", false).as_bool());
        form.explain("Add or edit destinations one per line. Remove a line to delete that destination; an empty list disables forwarding.");
    } else if (resource == "aliases") {
        form.text_field("address", "Alias address", string_field(record, "address"), 128);
        std::vector<std::pair<std::string, Value>> choices;
        const auto owner = record_id(mailbox_->account.at("organization_id"));
        for (const auto& account : inventory_.accounts)
            if (account.organization_id == owner) choices.emplace_back(account.address, account.id);
        form.choice("account_id", "Deliver to mailbox", std::move(choices), field(record, "account_id", selected_id_));
    } else if (resource == "dns") {
        form.choice("replace", "Operation", {{"Create only", false}, {"Replace complete mail configuration", true}}, record.contains("desired"));
        const auto value = field(record, "desired", field(record, "default_configuration", Object{}));
        form.configuration("configuration", "Complete DNS configuration (JSON)", value.is_object() ? value.as_object() : Object{});
        form.explain("Replacement submits the complete configuration. Preserve every DKIM selector and policy you still need. MTA-STS requires an existing trusted HTTPS policy host.");
    }
    status_->setText("Editing. Click Save to apply changes or Cancel to discard them.");
    busy();
}

void Application::remove(std::string resource, std::string id, std::string name) {
    if (!ensure_authenticated() || !credential_ || pending_ || editor_) return;
    details_->clear(); notice_->clear(); uncertain_ = false;
    editor_ = details_->addWidget(std::make_unique<Editor>("Delete " + name + "?",
        [this, resource, id](Object) { submit("DELETE", resource, id, {}); },
        [this] { render_selected(); }, true));
    editor_->explain(resource == "accounts" ? "Login is removed immediately. Mailbox files are cleaned up in the background; this cannot be undone." : "Confirm deletion. Dependencies must be removed first; DNS retirement may finish later.");
    busy();
}
void Application::submit(std::string method, std::string resource, std::string id, Object input) {
    if (!ensure_authenticated() || !credential_ || pending_ || uncertain_) return;
    status_->setText("Saving…");
    pending_ = client_->mutate(credential_, std::move(method), std::move(resource), std::move(id), std::move(input));
    busy();
}
void Application::mutation_finished(Mutation mutation) {
    editor_ = nullptr; uncertain_ = false;
    details_->clear(); notice_->clear(); notice_->setStyleClass("notice");
    label(notice_, "Saved. Courier activation and DNS work may still be pending; inspect DNS settings for its current status.");
    if (const auto* password = mutation.reply.if_contains("generated_password"); password && password->is_string()) {
        label(notice_, "Generated password — copy it now: " + text(*password), "field-value");
        notice_->addWidget(std::make_unique<Wt::WPushButton>("Dismiss password"))->clicked().connect([this] { notice_->clear(); });
    }
    if (const auto* account = mutation.reply.if_contains("account")) reload_selection_ = Selection{"accounts", std::to_string(record_id(account->at("id")))};
    else if (const auto* organization = mutation.reply.if_contains("organization")) reload_selection_ = Selection{"organizations", std::to_string(record_id(organization->at("id")))};
    else if (const auto* domain = mutation.reply.if_contains("domain"); domain && domain->is_object()) reload_selection_ = Selection{"domains", text(domain->at("domain"))};
    else if (const auto* alias = mutation.reply.if_contains("alias")) reload_selection_ = Selection{"accounts", std::to_string(record_id(alias->at("account_id")))};
    else if (mutation.reply.contains("dns")) {
        // DNS is independently durable and can remain pending through a long
        // MTA-STS retirement. Display its report rather than claiming success.
        document_ = Document{"dns", std::move(mutation.reply)}; mailbox_.reset();
        display_document(*document_); return;
    } else if (selected_id_ && !mutation.reply.contains("deleted")) reload_selection_ = selected_;
    mailbox_.reset(); document_.reset();
    refresh();
}
}
