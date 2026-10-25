#include "Data.h"

#include <algorithm>
#include <map>
#include <set>
#include <stdexcept>
#include <string_view>

namespace mailadmin::ui {
namespace {
[[noreturn]] void malformed() { throw std::runtime_error{"Unexpected API response; refresh the inventory"}; }

const boost::json::array& records(const Value& value, std::string_view field) {
    const auto& result = value.at(field).as_array();
    if (result.size() > 10000) malformed();
    return result;
}
std::string string(const Value& value) {
    const auto& result = value.as_string();
    if (result.size() > 16384) malformed();
    return {result.data(), result.size()};
}
void policy(const Object& item, bool inherited) {
    for (const auto* field : {"filter_enabled", "spam_enabled", "spam_folder", "spam_retention_days", "spam_trash"}) {
        const auto& value = item.at(field);
        if (value.is_null() && inherited) continue;
        const std::string name{field};
        if (name == "filter_enabled" || name == "spam_enabled") (void)value.as_bool();
        else if (name == "spam_retention_days") {
            const auto days = record_id(value);
            if (days > 36500) malformed();
        } else (void)string(value);
    }
}
}

std::int64_t record_id(const Value& value) {
    // The API's identifiers are JSON integers, never paths supplied by a browser.
    if (!value.is_int64() && !value.is_uint64()) malformed();
    const auto id = boost::json::value_to<std::int64_t>(value);
    if (id <= 0 || id > 2147483647) malformed();
    return id;
}
std::string text(const Value& value) {
    if (value.is_null()) return "Inherit from organization";
    if (value.is_string()) return string(value);
    if (value.is_bool()) return value.as_bool() ? "Yes" : "No";
    if (value.is_int64() || value.is_uint64()) return boost::json::serialize(value);
    malformed();
}
std::string address_domain(const std::string& address) {
    const auto at = address.rfind('@');
    if (at == std::string::npos || at == 0 || at + 1 == address.size()) malformed();
    return address.substr(at + 1);
}

Inventory parse_inventory(const Value& organizations, const Value& domains, const Value& accounts) {
    Inventory result;
    std::set<std::int64_t> owners, account_ids;
    std::set<std::string> addresses;
    std::map<std::string, std::int64_t> domain_owners;
    for (const auto& item : records(organizations, "organizations")) {
        const auto id = record_id(item.at("id"));
        if (!owners.insert(id).second) malformed();
        result.organizations.push_back({id, string(item.at("name"))});
    }
    for (const auto& item : records(domains, "domains")) {
        auto name = string(item.at("domain"));
        const auto owner = record_id(item.at("organization_id"));
        if (!owners.contains(owner) || name.empty() || !domain_owners.emplace(name, owner).second) malformed();
        result.domains.push_back({std::move(name), owner});
    }
    for (const auto& item : records(accounts, "accounts")) {
        auto address = string(item.at("address"));
        const auto owner = record_id(item.at("organization_id"));
        const auto id = record_id(item.at("id"));
        const auto domain = domain_owners.find(address_domain(address));
        if (!owners.contains(owner) || domain == domain_owners.end() || domain->second != owner
            || !account_ids.insert(id).second || !addresses.insert(address).second) malformed();
        result.accounts.push_back({id, owner, std::move(address)});
    }
    std::ranges::sort(result.organizations, {}, &Organization::name);
    std::ranges::sort(result.domains, {}, &Domain::name);
    std::ranges::sort(result.accounts, {}, &Account::address);
    return result;
}

Mailbox parse_mailbox(const Value& account, const Value& organization, const Value& aliases) {
    Mailbox result{account.at("account").as_object(), organization.at("organization").as_object(), {}, {}};
    const auto account_id = record_id(result.account.at("id"));
    const auto owner = record_id(result.account.at("organization_id"));
    if (record_id(result.organization.at("id")) != owner) malformed();
    (void)address_domain(string(result.account.at("address")));
    for (const auto* field : {"address", "name", "quota", "auto_reply"}) (void)string(result.account.at(field));
    for (const auto* field : {"active", "desired_active", "keep_copy"}) (void)result.account.at(field).as_bool();
    for (const auto* field : {"name", "default_domain", "inbound_mailserver", "outbound_mailserver"})
        (void)string(result.organization.at(field));
    (void)result.organization.at("hosted").as_bool();
    const auto& forwards = result.account.at("forwards").as_array();
    if (forwards.size() > 32) malformed();
    for (const auto& forward : forwards) (void)address_domain(string(forward));
    policy(result.account, true);
    policy(result.organization, false);
    std::set<std::string> seen;
    for (const auto& alias : records(aliases, "aliases")) {
        if (record_id(alias.at("account_id")) != account_id) continue;
        if (record_id(alias.at("organization_id")) != owner) malformed();
        auto address = string(alias.at("address"));
        (void)address_domain(address);
        if (!seen.insert(address).second) malformed();
        result.aliases.push_back(std::move(address));
        result.alias_records.push_back(alias);
    }
    std::ranges::sort(result.aliases);
    return result;
}

}
