#pragma once

#include <boost/json.hpp>
#include <cstdint>
#include <string>
#include <vector>

namespace mailadmin::ui {

using Object = boost::json::object;
using Value = boost::json::value;

struct Organization {
    std::int64_t id;
    std::string name;
};
struct Domain {
    std::string name;
    std::int64_t organization_id;
};
struct Account {
    std::int64_t id;
    std::int64_t organization_id;
    std::string address;
};
struct Inventory {
    std::vector<Organization> organizations;
    std::vector<Domain> domains;
    std::vector<Account> accounts;
};
struct Mailbox {
    Object account;
    Object organization;
    std::vector<std::string> aliases;
    boost::json::array alias_records;
};
struct Document { std::string resource; Object value; };
struct Mutation { Object reply; };

// Decode only the documented API contract; never render arbitrary JSON/HTML.
Inventory parse_inventory(const Value& organizations, const Value& domains, const Value& accounts);
Mailbox parse_mailbox(const Value& account, const Value& organization, const Value& aliases);
std::int64_t record_id(const Value& value);
std::string text(const Value& value);
std::string address_domain(const std::string& address);

}
