#include "Database.h"
#include "logging.h"
#include "Sql.h"
#include <mysql.h>
#include <yahat/Metrics.h>
#include <charconv>
#include <limits>
#include <set>
#include <sstream>
#include <vector>

namespace mailadmin {
std::int64_t identifier(std::string_view value) {
    std::int64_t result = 0;
    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), result);
    if (error != std::errc{} || end != value.data() + value.size() || result <= 0)
        throw ApiError{409, "inventory_conflict", "Invalid organization identity in legacy data"};
    return result;
}
std::string domain_name(std::string value) {
    if (!value.empty() && value.back() == '.') value.pop_back();
    if (value.empty() || value.size() > 253)
        throw ApiError{409, "inventory_conflict", "Invalid domain in legacy data"};
    std::size_t label_length = 0;
    char previous = 0;
    for (auto& character : value) {
        if (character >= 'A' && character <= 'Z') character += 'a' - 'A';
        if (character == '.') {
            if (!label_length || previous == '-') throw ApiError{409, "inventory_conflict", "Invalid domain label"};
            label_length = 0;
        } else {
            if (!((character >= 'a' && character <= 'z') || (character >= '0' && character <= '9')
                  || (character == '-' && label_length)))
                throw ApiError{409, "inventory_conflict", "Invalid domain character"};
            if (++label_length > 63) throw ApiError{409, "inventory_conflict", "Invalid domain label length"};
        }
        previous = character;
    }
    if (!label_length || previous == '-') throw ApiError{409, "inventory_conflict", "Invalid domain label"};
    return value;
}
std::string address_domain(const std::string& value) {
    const auto separator = value.rfind('@');
    if (separator == std::string::npos || separator == 0)
        throw ApiError{409, "inventory_conflict", "Unqualified address in legacy inventory"};
    return domain_name(value.substr(separator + 1));
}

boost::asio::awaitable<Snapshot> read_inventory(std::shared_ptr<const Config> config) {
    Connection connection{*config};
    Transaction transaction{connection};
    // Fixed queries deliberately never select clear, crypt, auto-reply text or
    // message data. MyISAM readers may see concurrent legacy changes; conflict
    // detection fails closed. InnoDB takeover will allow consistent snapshots.
    const auto organizations = connection.query(
        "SELECT organization_id,name,default_domain,hosted_here FROM organization ORDER BY organization_id LIMIT 10001");
    Snapshot snapshot;
    std::map<std::int64_t, bool> hosted;
    auto add_domain = [&](std::string name, std::int64_t owner) -> Domain& {
        if (!hosted.contains(owner)) throw ApiError{409, "inventory_conflict", "Missing organization in legacy inventory"};
        auto [entry, inserted] = snapshot.domains.try_emplace(std::move(name), Domain{owner, hosted.at(owner), 0, 0});
        if (!inserted && entry->second.organization_id != owner)
            throw ApiError{409, "inventory_conflict", "Domain is associated with multiple organizations"};
        return entry->second;
    };
    for (const auto& row : organizations) {
        const auto id = identifier(row[0]);
        if (!hosted.emplace(id, row[3] == "yes").second)
            throw ApiError{409, "inventory_conflict", "Duplicate organization identity"};
        const auto name = domain_name(row[2]);
        snapshot.organizations.emplace_back(boost::json::object{
            {"id", id}, {"name", row[1]}, {"default_domain", name}, {"hosted", hosted.at(id)}});
        add_domain(name, id);
    }
    for (const auto& row : connection.query(
            "SELECT id,organization_id FROM passwd ORDER BY passwd_id LIMIT 10001")) {
        ++add_domain(address_domain(row[0]), identifier(row[1])).accounts;
    }
    for (const auto& row : connection.query(
            "SELECT a.mailaddr,a.organization_id,p.organization_id FROM alias a LEFT JOIN passwd p ON p.passwd_id=a.passwd_id ORDER BY a.alias_id LIMIT 10001")) {
        const auto owner = identifier(row[1]);
        if (owner != identifier(row[2])) throw ApiError{409, "inventory_conflict", "Alias has an inconsistent organization"};
        ++add_domain(address_domain(row[0]), owner).aliases;
    }
    if (config->administration) {
        for (const auto& row : connection.query("SELECT name,organization_id FROM domain ORDER BY name"))
            add_domain(row[0], identifier(row[1]));
    }
    transaction.commit();
    MA_DEBUG << "Inventory read: organizations=" << snapshot.organizations.size() << " domains=" << snapshot.domains.size();
    co_return snapshot;
}
boost::json::array Snapshot::domain_json() const {
    boost::json::array result;
    for (const auto& [name, domain] : domains) {
        result.emplace_back(boost::json::object{{"domain", name}, {"organization_id", domain.organization_id},
            {"hosted", domain.hosted}, {"email_accounts", domain.accounts}, {"aliases", domain.aliases}});
    }
    return result;
}
std::string Snapshot::metrics() const {
    yahat::Metrics metrics;
    for (const auto& [name, domain] : domains) {
        if (!domain.hosted) continue;
        metrics.AddGauge<std::uint64_t>("mailadmin_email_accounts", "Configured email accounts", {}, {{"domain", name}})->set(domain.accounts);
        metrics.AddGauge<std::uint64_t>("mailadmin_aliases", "Configured alias addresses", {}, {{"domain", name}})->set(domain.aliases);
    }
    std::ostringstream result;
    metrics.generate(result);
    return result.str();
}
}
