#include "Data.h"
#include <functional>
#include <iostream>
#include <stdexcept>

using namespace mailadmin::ui;
namespace {
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error{message};
}
void rejects(const std::function<void()>& operation) {
    bool rejected = false;
    try { operation(); } catch (const std::exception&) { rejected = true; }
    require(rejected, "Malformed or inconsistent API data was accepted");
}
Value json(const char* value) { return boost::json::parse(value); }
}

int main() {
    try {
        auto organizations = json(R"({"organizations":[{"id":2,"name":"Zeta"},{"id":1,"name":"Alpha <b>literal</b>"}]})");
        auto domains = json(R"({"domains":[{"domain":"empty.test","organization_id":2},{"domain":"mail.test","organization_id":1}]})");
        auto accounts = json(R"({"accounts":[{"id":8,"address":"z@mail.test","organization_id":1},{"id":7,"address":"a@mail.test","organization_id":1}]})");
        const auto inventory = parse_inventory(organizations, domains, accounts);
        require(inventory.organizations.front().id == 1, "Organizations were not sorted");
        require(inventory.organizations.front().name == "Alpha <b>literal</b>", "API text was altered");
        require(inventory.domains.size() == 2 && inventory.accounts.front().id == 7, "Empty domain or account ordering lost");
        auto bad_domains = domains;
        bad_domains.at("domains").as_array()[0].at("organization_id") = 99;
        rejects([&] { (void)parse_inventory(organizations, bad_domains, accounts); });
        auto bad_accounts = accounts;
        bad_accounts.at("accounts").as_array()[0].at("organization_id") = 2;
        rejects([&] { (void)parse_inventory(organizations, domains, bad_accounts); });
        auto duplicate = organizations;
        duplicate.at("organizations").as_array().push_back(duplicate.at("organizations").as_array()[0]);
        rejects([&] { (void)parse_inventory(duplicate, domains, accounts); });
        auto deleted_domain = json(R"({"domains":[]})");
        rejects([&] { (void)parse_inventory(organizations, deleted_domain, accounts); });
        rejects([] { (void)record_id(Value(-1)); });
        rejects([] { (void)record_id(Value(1.0)); });
        rejects([] { (void)record_id(Value("1/../../accounts")); });

        auto account = json(R"({"account":{"id":7,"organization_id":1,"address":"a@mail.test","name":"<script>literal</script>","quota":"10000000S,1000C","active":false,"desired_active":true,"keep_copy":true,"auto_reply":"Away\nBack tomorrow","forwards":["b@example.test"],"filter_enabled":null,"spam_enabled":null,"spam_folder":"junk","spam_retention_days":null,"spam_trash":""}})");
        auto organization = json(R"({"organization":{"id":1,"name":"Alpha","default_domain":"mail.test","hosted":true,"inbound_mailserver":"mx.mail.test","outbound_mailserver":"smtp.mail.test","filter_enabled":true,"spam_enabled":false,"spam_folder":"spam","spam_retention_days":30,"spam_trash":"Trash"}})");
        auto aliases = json(R"({"aliases":[{"account_id":8,"organization_id":1,"address":"other@mail.test"},{"account_id":7,"organization_id":1,"address":"z@other.test"},{"account_id":7,"organization_id":1,"address":"alias@mail.test"}]})");
        const auto mailbox = parse_mailbox(account, organization, aliases);
        require(mailbox.aliases.size() == 2 && mailbox.aliases[0] == "alias@mail.test", "Aliases leaked across accounts or lost sorting");
        require(mailbox.account.at("filter_enabled").is_null(), "Inherited policy was flattened");
        require(text(mailbox.account.at("spam_trash")).empty(), "Delete policy was lost");
        require(text(mailbox.account.at("name")) == "<script>literal</script>", "Plain text was not preserved");
        require(!mailbox.account.at("active").as_bool() && mailbox.account.at("desired_active").as_bool(), "Activation states were conflated");
        organization.at("organization").at("id") = 2;
        rejects([&] { (void)parse_mailbox(account, organization, aliases); });
        organization.at("organization").at("id") = 1;
        aliases.at("aliases").as_array()[1].at("organization_id") = 2;
        rejects([&] { (void)parse_mailbox(account, organization, aliases); });
        std::cout << "UI API contract checks passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
