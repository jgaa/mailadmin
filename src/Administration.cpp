#include "Administration.h"
#include "Security.h"
#include "logging.h"
#include <openssl/crypto.h>
#include <algorithm>
#include <charconv>
#include <set>
#include <unistd.h>

namespace mailadmin {
namespace {
using Object = boost::json::object;
using Value = boost::json::value;
using Method = yahat::Request::Type;
[[noreturn]] void invalid(std::string message) { throw ApiError{400, "invalid_argument", std::move(message)}; }
[[noreturn]] void missing() { throw ApiError{404, "not_found", "Resource not found"}; }
void fields(const Object& object, std::initializer_list<std::string_view> allowed) {
    for (const auto& field : object)
        if (std::find(allowed.begin(), allowed.end(), field.key()) == allowed.end()) invalid("Unknown JSON field");
}
std::string text(const Object& object, std::string_view field, std::size_t limit, bool required = true) {
    const auto* value = object.if_contains(field);
    if (!value || !value->is_string()) invalid("A required string is missing or has the wrong type");
    std::string result{value->as_string()};
    if ((required && result.empty()) || result.size() > limit || result.find('\0') != std::string::npos)
        invalid("String length or encoding is invalid");
    return result;
}
std::int64_t number(const Value& value) {
    if (!value.is_int64() || value.as_int64() < 1 || value.as_int64() > 2147483647) invalid("Invalid resource ID");
    return value.as_int64();
}
bool boolean(const Value& value) { if (!value.is_bool()) invalid("Expected a boolean"); return value.as_bool(); }
std::string yes(bool value) { return value ? "yes" : "no"; }
void method(bool allowed) { if (!allowed) throw ApiError{405, "method_not_allowed", "Method is not supported for this resource"}; }
void exists(Connection& db, std::string_view table, const std::string& id) {
    // Table identifiers are exclusively literals selected by the implementation.
    const std::string key = table == "passwd" ? "passwd_id" : "organization_id";
    if (db.query("SELECT " + key + " FROM " + std::string{table} + " WHERE " + key + "=?", {id}).empty()) missing();
}
void dirty(Connection& db) { db.query("UPDATE admin_state SET generation=generation+1,last_error='' WHERE singleton=1"); }
Object activation(Connection& db) {
    const auto row = db.query("SELECT generation,applied_generation,last_error FROM admin_state WHERE singleton=1").at(0);
    return {{"generation", std::stoll(row[0])}, {"applied_generation", std::stoll(row[1])},
        {"status", row[0] == row[1] ? "active" : "pending"}, {"error", row[2]}};
}
const std::map<std::string, std::string> policy_columns{
    {"filter_enabled","filterctl"}, {"spam_enabled","spammgt"}, {"spam_folder","spamfld"},
    {"spam_retention_days","spamdays"}, {"spam_trash","spamtrash"}};
void policy(Connection& db, std::string_view table, const std::string& id, const Object& object) {
    for (const auto& [field, column] : policy_columns) {
        if (const auto* value = object.if_contains(field)) {
            std::optional<std::string> content;
            if (value->is_null()) {
                if (table == "organization") invalid("Organization policies cannot inherit");
            } else if (field == "filter_enabled" || field == "spam_enabled") content = yes(boolean(*value));
            else if (field == "spam_retention_days") {
                if (!value->is_int64() || value->as_int64() < 1 || value->as_int64() > 36500)
                    invalid("Spam retention must be between 1 and 36500 days");
                content = std::to_string(value->as_int64());
            } else content = folder_name(text(object, field, 64, field != "spam_trash"), field == "spam_trash");
            db.query("UPDATE " + std::string{table} + " SET " + column + "=? WHERE "
                + (table == "passwd" ? "passwd_id" : "organization_id") + "=?", {content, id});
        }
    }
}
void attach_policy(Object& object, const std::vector<std::string>& row, std::size_t offset, bool inherited) {
    for (const auto& field : {"filter_enabled", "spam_enabled", "spam_folder", "spam_retention_days", "spam_trash"}) {
        const auto& value = row[offset++];
        if (inherited && value == "<inherit>") object[field] = nullptr;
        else if (std::string_view{field}.ends_with("enabled")) object[field] = value == "yes";
        else if (std::string_view{field} == "spam_retention_days") object[field] = std::stoi(value);
        else object[field] = value;
    }
}
boost::json::array organizations(Connection& db, const std::string& id = {}) {
    auto rows = db.query("SELECT organization_id,name,default_domain,inbound_mailserver,outbound_mailserver,hosted_here,"
        "filterctl,spammgt,spamfld,spamdays,spamtrash FROM organization" + (id.empty() ? std::string{} : " WHERE organization_id=?")
        + " ORDER BY organization_id", id.empty() ? Parameters{} : Parameters{id});
    boost::json::array result;
    for (const auto& row : rows) {
        Object item{{"id", std::stoll(row[0])}, {"name", row[1]}, {"default_domain", row[2]},
            {"inbound_mailserver",row[3]}, {"outbound_mailserver",row[4]}, {"hosted",row[5] == "yes"}};
        attach_policy(item, row, 6, false);
        result.emplace_back(std::move(item));
    }
    return result;
}
boost::json::array accounts(Connection& db, const std::string& id = {}) {
    auto rows = db.query("SELECT p.passwd_id,p.id,p.organization_id,p.name,p.quota,p.active,p.keep_copy,COALESCE(p.oomsg,''),"
        "COALESCE(p.filterctl,'<inherit>'),COALESCE(p.spammgt,'<inherit>'),COALESCE(p.spamfld,'<inherit>'),"
        "COALESCE(CAST(p.spamdays AS CHAR),'<inherit>'),COALESCE(p.spamtrash,'<inherit>'),"
        "COALESCE(a.desired_active,p.active) FROM passwd p LEFT JOIN admin_account a ON a.passwd_id=p.passwd_id"
        + (id.empty() ? std::string{} : " WHERE p.passwd_id=?") + " ORDER BY p.passwd_id", id.empty() ? Parameters{} : Parameters{id});
    boost::json::array result;
    for (const auto& row : rows) {
        Object item{{"id",std::stoll(row[0])}, {"address",row[1]}, {"organization_id",std::stoll(row[2])},
            {"name",row[3]}, {"quota",row[4]}, {"active",row[5] == "yes"}, {"desired_active",row[13] == "yes"},
            {"keep_copy",row[6] == "yes"}, {"auto_reply",row[7]}};
        attach_policy(item, row, 8, true);
        boost::json::array forwards;
        for (const auto& forward : db.query("SELECT destination FROM forward WHERE passwd_id=? ORDER BY forward_id", {row[0]}))
            forwards.emplace_back(forward[0]);
        item["forwards"] = std::move(forwards);
        result.emplace_back(std::move(item));
    }
    return result;
}
std::string owner_of(Connection& db, const std::string& address) {
    auto rows = db.query("SELECT organization_id FROM domain WHERE name=?", {address_domain(address)});
    if (rows.empty()) throw ApiError{409, "domain_required", "Create the owning domain before using this address"};
    return rows[0][0];
}
void retire_address(Connection& db, const std::string& address) {
    db.query("INSERT INTO admin_retired_address(address,generation) SELECT ?,generation+1 FROM admin_state WHERE singleton=1 ON DUPLICATE KEY UPDATE generation=GREATEST(admin_retired_address.generation,VALUES(generation))",{mailbox_address(address)});
}
void unused_address(Connection& db, const std::string& address, const std::string& except = "0") {
    if (!db.query("SELECT address FROM admin_retired_address WHERE address=? AND generation>(SELECT applied_generation FROM admin_state WHERE singleton=1)",{address}).empty())
        throw ApiError{409,"address_retiring","Wait for Courier activation before reusing this address"};
    if (!db.query("SELECT passwd_id FROM passwd WHERE id=? AND passwd_id<>?", {address,except}).empty()
        || !db.query("SELECT alias_id FROM alias WHERE mailaddr=?", {address}).empty())
        throw ApiError{409, "address_in_use", "This mail address is already in use"};
}
std::string numeric_path_id(std::string_view value) {
    std::int64_t id = 0;
    const auto [end,error] = std::from_chars(value.data(), value.data()+value.size(), id);
    if (error != std::errc{} || end != value.data()+value.size() || id <= 0 || id > 2147483647) invalid("Invalid resource ID");
    return std::to_string(id);
}
}
std::string mailbox_address(std::string value) {
    if (value.size() > 128) invalid("Address exceeds 128 characters");
    const auto separator = value.find('@');
    if (separator == std::string::npos || separator == 0 || separator > 64 || separator != value.rfind('@')) invalid("Invalid mail address");
    for (std::size_t index = 0; index < separator; ++index) {
        auto& character = value[index];
        if (character >= 'A' && character <= 'Z') character += 'a'-'A';
        if (!((character >= 'a' && character <= 'z') || (character >= '0' && character <= '9')
            || character == '_' || character == '-' || character == '+' || character == '.' || character == '='))
            invalid("Unsupported local part in mail address");
    }
    if (value.front() == '-' || value.front() == '=' || value.front() == '.' || value[separator-1] == '.' || value.substr(0,separator).find("..") != std::string::npos)
        invalid("Invalid local part in mail address");
    try { return value.substr(0,separator+1) + domain_name(value.substr(separator+1)); }
    catch (const ApiError&) { invalid("Invalid mail domain"); }
}
std::string folder_name(std::string value, bool allow_empty) {
    if ((value.empty() && !allow_empty) || value.size() > 64) invalid("Invalid Maildir folder name");
    for (const auto character : value)
        if (!((character >= 'A' && character <= 'Z') || (character >= 'a' && character <= 'z')
            || (character >= '0' && character <= '9') || character == '-' || character == '_'))
            invalid("Folder names must use letters, numbers, underscores or hyphens");
    return value;
}

void quota_value(std::string_view quota) {
    if (!quota.empty()) {
        bool size_seen = false, count_seen = false;
        std::size_t begin = 0;
        while (begin < quota.size()) {
            const auto end = quota.find(',',begin);
            const auto part = quota.substr(begin,end == std::string::npos ? end : end-begin);
            if (part.size()<2 || part.size()>20) invalid("Invalid Courier quota");
            for (std::size_t index=0;index+1<part.size();++index)
                if (part[index]<'0'||part[index]>'9') invalid("Invalid Courier quota");
            if (part.back()=='S' && !size_seen) size_seen=true;
            else if (part.back()=='C' && !count_seen) count_seen=true;
            else invalid("Invalid Courier quota");
            if (end == std::string::npos) break;
            begin=end+1;
            if (begin==quota.size()) invalid("Invalid Courier quota");
        }
    }
}

void migrate_schema(const Config& config) {
    Connection db{config};
    AdministrationLock lock{db};
    const auto engines = db.query("SELECT TABLE_NAME,ENGINE FROM information_schema.TABLES WHERE TABLE_SCHEMA=DATABASE() AND TABLE_NAME IN ('organization','passwd','alias','forward')");
    if (engines.size() != 4 || std::any_of(engines.begin(), engines.end(), [](const auto& row) { return row[1] != "InnoDB"; }))
        throw ApiError{409, "innodb_required", "Back up the database and convert all four legacy tables to InnoDB first"};
    if (!db.query("SELECT organization_id FROM organization GROUP BY organization_id HAVING COUNT(*)<>1").empty())
        throw ApiError{409, "inventory_conflict", "Organization IDs must be unique"};
    std::map<std::string,std::string> domains;
    std::set<std::string> owners;
    for (const auto& row : db.query("SELECT organization_id FROM organization")) owners.insert(row[0]);
    for (const auto& row : db.query("SELECT default_domain,organization_id FROM organization UNION ALL SELECT SUBSTRING_INDEX(id,'@',-1),organization_id FROM passwd UNION ALL SELECT SUBSTRING_INDEX(mailaddr,'@',-1),organization_id FROM alias")) {
        const auto domain = domain_name(row[0]);
        if (!owners.contains(row[1])) throw ApiError{409,"inventory_conflict","Missing organization"};
        auto [entry, inserted] = domains.emplace(domain,row[1]);
        if (!inserted && entry->second != row[1]) throw ApiError{409,"inventory_conflict","Domain has multiple owners"};
    }
    if (!db.query("SELECT a.alias_id FROM alias a LEFT JOIN passwd p ON p.passwd_id=a.passwd_id WHERE p.passwd_id IS NULL OR a.organization_id<>p.organization_id").empty()
        || !db.query("SELECT a.alias_id FROM alias a JOIN passwd p ON p.id=a.mailaddr").empty()
        || !db.query("SELECT f.forward_id FROM forward f LEFT JOIN passwd p ON p.passwd_id=f.passwd_id WHERE p.passwd_id IS NULL").empty())
        throw ApiError{409,"inventory_conflict","Legacy aliases or forwards are inconsistent"};
    // DDL commits implicitly. Each step is idempotent; no data is removed. The
    // operator runs this offline after a backup and stopping legacy writers.
    if (db.query("SELECT INDEX_NAME FROM information_schema.STATISTICS WHERE TABLE_SCHEMA=DATABASE() AND TABLE_NAME='organization' AND COLUMN_NAME='organization_id' AND NON_UNIQUE=0").empty())
        db.query("ALTER TABLE organization ADD UNIQUE KEY mailadmin_organization_id(organization_id)");
    db.query("ALTER TABLE organization MODIFY default_domain VARCHAR(253) NOT NULL");
    db.query("CREATE TABLE IF NOT EXISTS domain (name VARCHAR(253) CHARACTER SET ascii COLLATE ascii_bin PRIMARY KEY, organization_id INT NOT NULL, INDEX(organization_id), CONSTRAINT mailadmin_domain_owner FOREIGN KEY(organization_id) REFERENCES organization(organization_id)) ENGINE=InnoDB");
    db.query("CREATE TABLE IF NOT EXISTS admin_state (singleton INT PRIMARY KEY, generation BIGINT NOT NULL DEFAULT 1, applied_generation BIGINT NOT NULL DEFAULT 0, last_error VARCHAR(64) NOT NULL DEFAULT '') ENGINE=InnoDB");
    db.query("CREATE TABLE IF NOT EXISTS admin_account (passwd_id INT PRIMARY KEY, desired_active ENUM('yes','no') NOT NULL, CONSTRAINT mailadmin_account FOREIGN KEY(passwd_id) REFERENCES passwd(passwd_id) ON DELETE CASCADE) ENGINE=InnoDB");
    db.query("CREATE TABLE IF NOT EXISTS admin_retired_address (address VARCHAR(128) CHARACTER SET ascii COLLATE ascii_bin PRIMARY KEY, generation BIGINT NOT NULL) ENGINE=InnoDB");
    db.query("CREATE TABLE IF NOT EXISTS admin_cleanup (passwd_id INT PRIMARY KEY, organization_id INT NOT NULL, home VARCHAR(255) NOT NULL, generation BIGINT NOT NULL, attempts INT NOT NULL DEFAULT 0, last_error VARCHAR(64) NOT NULL DEFAULT '') ENGINE=InnoDB");
    Transaction transaction{db};
    db.query("INSERT IGNORE INTO admin_state(singleton) VALUES(1)");
    for (const auto& [domain,owner] : domains) {
        const auto existing = db.query("SELECT organization_id FROM domain WHERE name=?", {domain});
        if (!existing.empty() && existing[0][0] != owner) throw ApiError{409,"inventory_conflict","Explicit domain ownership conflicts with legacy data"};
        db.query("INSERT IGNORE INTO domain(name,organization_id) VALUES(?,?)", {domain,owner});
    }
    db.query("INSERT IGNORE INTO admin_account(passwd_id,desired_active) SELECT passwd_id,active FROM passwd");
    transaction.commit();
    MA_INFO << "Administration schema is ready; domains=" << domains.size();
}
void migrate_passwords(const Config& config) {
    Connection db{config};
    AdministrationLock lock{db};
    if (db.query("SELECT ENGINE FROM information_schema.TABLES WHERE TABLE_SCHEMA=DATABASE() AND TABLE_NAME='passwd'") != Rows{{"InnoDB"}})
        throw ApiError{409,"innodb_required","Password conversion requires an InnoDB passwd table"};
    Transaction transaction{db};
    auto rows = db.query("SELECT passwd_id,clear,crypt FROM passwd WHERE clear<>'' FOR UPDATE");
    struct WipeRows {
        Rows& rows;
        ~WipeRows() { for (auto& row : rows) OPENSSL_cleanse(row[1].data(),row[1].size()); }
    } wipe{rows};
    std::size_t count = 0;
    for (auto& row : rows) {
        if (!row[2].empty() && !password_matches(row[1],row[2]))
            throw ApiError{409,"password_conflict","A stored plaintext password does not match its existing hash; no passwords changed"};
        auto hash = row[2].empty() ? password_hash(row[1]) : row[2];
        db.query("UPDATE passwd SET crypt=?,clear='' WHERE passwd_id=?", {hash,row[0]});
        OPENSSL_cleanse(row[1].data(),row[1].size());
        ++count;
    }
    transaction.commit();
    MA_INFO << "Converted plaintext passwords; accounts=" << count;
}

boost::asio::awaitable<Operation> administer(std::shared_ptr<const Config> config, Method method_type,
        std::string target, std::string body) {
    if (!config->administration) throw ApiError{503,"administration_disabled","Administration is not enabled on this server"};
    Object input;
    struct WipeBody {
        std::string& body;
        Object& object;
        ~WipeBody() {
            OPENSSL_cleanse(body.data(),body.size());
            if (auto* password = object.if_contains("password"); password && password->is_string())
                OPENSSL_cleanse(password->as_string().data(),password->as_string().size());
        }
    } wipe{body,input};
    if (!body.empty()) {
        boost::system::error_code error;
        auto value = boost::json::parse(body,error);
        if (error || !value.is_object()) invalid("Request body must be a JSON object");
        input = std::move(value.as_object());
    }
    if ((method_type == Method::GET || method_type == Method::DELETE) && !body.empty()) invalid("This method does not accept a body");
    const std::string prefix = "/api/v1/";
    if (!target.starts_with(prefix)) missing();
    const auto remainder = target.substr(prefix.size());
    const auto slash = remainder.find('/');
    const auto resource = remainder.substr(0,slash);
    const auto raw_id = slash == std::string::npos ? std::string{} : remainder.substr(slash+1);
    if (raw_id.find('/') != std::string::npos || (slash != std::string::npos && raw_id.empty())) missing();
    const auto id = raw_id.empty() || resource == "domains" ? raw_id : numeric_path_id(raw_id);
    Connection db{*config};
    AdministrationLock lock{db};
    if (method_type != Method::GET && db.query("SELECT COUNT(*) FROM information_schema.TABLES WHERE TABLE_SCHEMA=DATABASE() AND ENGINE='InnoDB' AND TABLE_NAME IN ('organization','passwd','alias','forward','domain','admin_account','admin_state','admin_cleanup','admin_retired_address')").at(0).at(0) != "9")
        throw ApiError{409,"innodb_required","All administration tables must use InnoDB"};
    Transaction transaction{db};
    Operation result;
    Object output;
    bool changed = method_type != Method::GET;
    if (resource == "operations") {
        method(method_type == Method::GET && id.empty());
        boost::json::array cleanup;
        for (const auto& row : db.query("SELECT passwd_id,attempts,last_error FROM admin_cleanup ORDER BY passwd_id"))
            cleanup.emplace_back(Object{{"account_id",std::stoll(row[0])},{"attempts",std::stoi(row[1])},{"error",row[2]}});
        output = {{"cleanup",std::move(cleanup)}};
    } else if (resource == "reconcile") {
        method(method_type == Method::POST && id.empty());
        fields(input,{});
        // Force full regeneration even after an external operator repaired a
        // host problem. The persistent worker performs the actual operation.
    } else if (resource == "organizations") {
        if (method_type == Method::GET) {
            auto items = organizations(db,id);
            if (!id.empty() && items.empty()) missing();
            if (id.empty()) output["organizations"] = std::move(items);
            else output["organization"] = std::move(items[0]);
        } else if (method_type == Method::DELETE) {
            method(!id.empty());
            exists(db,"organization",id);
            if (!db.query("SELECT passwd_id FROM passwd WHERE organization_id=? LIMIT 1",{id}).empty())
                throw ApiError{409,"organization_not_empty","Delete the organization's accounts first"};
            db.query("DELETE FROM domain WHERE organization_id=?",{id});
            db.query("DELETE FROM organization WHERE organization_id=?",{id});
            output["deleted"] = true;
        } else {
            method((method_type == Method::POST && id.empty()) || (method_type == Method::PATCH && !id.empty()));
            fields(input,{"name","default_domain","inbound_mailserver","outbound_mailserver","hosted",
                "filter_enabled","spam_enabled","spam_folder","spam_retention_days","spam_trash"});
            auto organization_id = id;
            if (id.empty()) {
                auto domain = domain_name(text(input,"default_domain",253));
                db.query("INSERT INTO organization(name,default_domain,inbound_mailserver,outbound_mailserver) VALUES(?,?,?,?)",
                    {text(input,"name",50),domain,"",""});
                organization_id = db.query("SELECT LAST_INSERT_ID()")[0][0];
                db.query("INSERT INTO domain(name,organization_id) VALUES(?,?)",{domain,organization_id});
                result.status = 201;
            } else exists(db,"organization",id);
            if (input.contains("name")) db.query("UPDATE organization SET name=? WHERE organization_id=?",{text(input,"name",50),organization_id});
            if (input.contains("default_domain")) {
                auto domain = domain_name(text(input,"default_domain",253));
                auto rows = db.query("SELECT organization_id FROM domain WHERE name=?",{domain});
                if (rows.empty() || rows[0][0] != organization_id)
                    throw ApiError{409,"domain_owner","Default domain must already belong to this organization"};
                db.query("UPDATE organization SET default_domain=? WHERE organization_id=?",{domain,organization_id});
            }
            for (const auto& field : {"inbound_mailserver","outbound_mailserver"}) {
                if (input.contains(field)) {
                    auto host = text(input,field,128,false);
                    if (!host.empty()) host = domain_name(host);
                    db.query("UPDATE organization SET " + std::string{field} + "=? WHERE organization_id=?",{host,organization_id});
                }
            }
            if (const auto* hosted = input.if_contains("hosted")) {
                db.query("UPDATE organization SET hosted_here=? WHERE organization_id=?",{yes(boolean(*hosted)),organization_id});
                if (!hosted->as_bool()) db.query("UPDATE passwd SET active='no' WHERE organization_id=?",{organization_id});
            }
            policy(db,"organization",organization_id,input);
            output["organization"] = std::move(organizations(db,organization_id)[0]);
        }
    } else if (resource == "domains") {
        const auto domain = id.empty() ? std::string{} : domain_name(id);
        const auto existing = domain.empty() ? Rows{} : db.query("SELECT organization_id FROM domain WHERE name=?",{domain});
        if (!domain.empty() && existing.empty()) missing();
        if (method_type == Method::GET) {
            boost::json::array items;
            for (const auto& row : db.query("SELECT name,organization_id FROM domain" + (domain.empty() ? std::string{} : " WHERE name=?") + " ORDER BY name",
                    domain.empty() ? Parameters{} : Parameters{domain}))
                items.emplace_back(Object{{"domain",row[0]},{"organization_id",std::stoll(row[1])}});
            if (domain.empty()) output["domains"] = std::move(items);
            else output["domain"] = std::move(items[0]);
        } else if (method_type == Method::POST || method_type == Method::PATCH) {
            method((method_type == Method::POST && domain.empty()) || (method_type == Method::PATCH && !domain.empty()));
            fields(input,domain.empty() ? std::initializer_list<std::string_view>{"domain","organization_id"}
                                      : std::initializer_list<std::string_view>{"organization_id"});
            if (!input.contains("organization_id")) invalid("organization_id is required");
            const auto owner = std::to_string(number(input.at("organization_id")));
            exists(db,"organization",owner);
            const auto name = domain.empty() ? domain_name(text(input,"domain",253)) : domain;
            if (domain.empty()) {
                db.query("INSERT INTO domain(name,organization_id) VALUES(?,?)",{name,owner});
                result.status = 201;
            } else {
                if (!db.query("SELECT passwd_id FROM passwd WHERE SUBSTRING_INDEX(id,'@',-1)=? LIMIT 1",{name}).empty()
                    || !db.query("SELECT alias_id FROM alias WHERE SUBSTRING_INDEX(mailaddr,'@',-1)=? LIMIT 1",{name}).empty()
                    || !db.query("SELECT organization_id FROM organization WHERE default_domain=? LIMIT 1",{name}).empty())
                    throw ApiError{409,"domain_in_use","Only an empty, non-default domain can change owner"};
                db.query("UPDATE domain SET organization_id=? WHERE name=?",{owner,name});
            }
            output["domain"] = Object{{"domain",name},{"organization_id",std::stoll(owner)}};
        } else {
            method(method_type == Method::DELETE && !domain.empty());
            if (!db.query("SELECT passwd_id FROM passwd WHERE SUBSTRING_INDEX(id,'@',-1)=? LIMIT 1",{domain}).empty()
                || !db.query("SELECT alias_id FROM alias WHERE SUBSTRING_INDEX(mailaddr,'@',-1)=? LIMIT 1",{domain}).empty()
                || !db.query("SELECT organization_id FROM organization WHERE default_domain=? LIMIT 1",{domain}).empty())
                throw ApiError{409,"domain_in_use","Remove domain addresses and change its default-domain reference first"};
            db.query("DELETE FROM domain WHERE name=?",{domain});
            output["deleted"] = true;
        }
    } else if (resource == "accounts") {
        if (method_type == Method::GET) {
            auto items = accounts(db,id);
            if (!id.empty() && items.empty()) missing();
            if (id.empty()) output["accounts"] = std::move(items);
            else output["account"] = std::move(items[0]);
        } else if (method_type == Method::DELETE) {
            method(!id.empty());
            auto rows = db.query("SELECT organization_id,home,id FROM passwd WHERE passwd_id=?",{id});
            if (rows.empty()) missing();
            // Record cleanup in the very same transaction that removes login,
            // forwarding and alias data. Never accept a filesystem path from HTTP.
            db.query("INSERT INTO admin_cleanup(passwd_id,organization_id,home,generation) SELECT ?,?,?,generation+1 FROM admin_state WHERE singleton=1",
                {id,rows[0][0],rows[0][1]});
            retire_address(db,rows[0][2]);
            for (const auto& alias : db.query("SELECT mailaddr FROM alias WHERE passwd_id=?",{id})) retire_address(db,alias[0]);
            db.query("DELETE FROM alias WHERE passwd_id=?",{id});
            db.query("DELETE FROM forward WHERE passwd_id=?",{id});
            db.query("DELETE FROM passwd WHERE passwd_id=?",{id});
            output["deleted"] = true;
        } else {
            method((method_type == Method::POST && id.empty()) || (method_type == Method::PATCH && !id.empty()));
            fields(input,{"address","name","quota","active","password","generate_password","keep_copy","auto_reply","forwards",
                "filter_enabled","spam_enabled","spam_folder","spam_retention_days","spam_trash"});
            auto account_id = id;
            if (id.empty()) {
                const auto address = mailbox_address(text(input,"address",128));
                unused_address(db,address);
                const auto owner = owner_of(db,address);
                db.query("INSERT INTO passwd(id,uid,gid,organization_id,active) VALUES(?,?,?,?,'no')",
                    {address,std::to_string(geteuid()),std::to_string(getegid()),owner});
                account_id = db.query("SELECT LAST_INSERT_ID()")[0][0];
                const auto home = (config->spool_root / owner / account_id).string();
                if (home.size()+8 > 255) invalid("Configured spool path is too long");
                db.query("UPDATE passwd SET home=?,maildir=? WHERE passwd_id=?",{home,home+"/Maildir",account_id});
                db.query("INSERT INTO admin_account(passwd_id,desired_active) VALUES(?,'yes')",{account_id});
                result.status = 201;
            } else exists(db,"passwd",id);
            if (!id.empty() && input.contains("address")) {
                const auto address = mailbox_address(text(input,"address",128));
                unused_address(db,address,id);
                const auto owner = owner_of(db,address);
                if (db.query("SELECT organization_id FROM passwd WHERE passwd_id=?",{id})[0][0] != owner)
                    throw ApiError{409,"owner_change","Account addresses must remain within their current organization"};
                const auto previous = db.query("SELECT id FROM passwd WHERE passwd_id=?",{id})[0][0];
                if (previous != address) retire_address(db,previous);
                db.query("UPDATE passwd SET id=? WHERE passwd_id=?",{address,id});
            }
            if (input.contains("name")) db.query("UPDATE passwd SET name=? WHERE passwd_id=?",{text(input,"name",128,false),account_id});
            if (input.contains("quota")) {
                const auto quota = text(input,"quota",255,false);
                quota_value(quota);
                db.query("UPDATE passwd SET quota=? WHERE passwd_id=?",{quota,account_id});
            }
            bool generate = id.empty() && !input.contains("password");
            if (const auto* value = input.if_contains("generate_password")) generate = boolean(*value);
            if (generate && input.contains("password")) invalid("Supply a password or request generation, not both");
            if (id.empty() && !generate && !input.contains("password")) invalid("New accounts require a password");
            if (generate || input.contains("password")) {
                auto password = generate ? random_secret() : text(input,"password",1024);
                if (password.size()<12) invalid("New passwords must contain at least 12 bytes");
                const auto hash = password_hash(password);
                db.query("UPDATE passwd SET crypt=?,clear='' WHERE passwd_id=?",{hash,account_id});
                if (generate) output["generated_password"] = password;
                OPENSSL_cleanse(password.data(),password.size());
            }
            if (const auto* active = input.if_contains("active")) {
                const auto enabled = yes(boolean(*active));
                db.query("INSERT INTO admin_account(passwd_id,desired_active) VALUES(?,?) ON DUPLICATE KEY UPDATE desired_active=VALUES(desired_active)",{account_id,enabled});
                if (!active->as_bool()) db.query("UPDATE passwd SET active='no' WHERE passwd_id=?",{account_id});
            }
            if (const auto* keep = input.if_contains("keep_copy")) db.query("UPDATE passwd SET keep_copy=? WHERE passwd_id=?",{yes(boolean(*keep)),account_id});
            if (input.contains("auto_reply")) db.query("UPDATE passwd SET oomsg=? WHERE passwd_id=?",{text(input,"auto_reply",8192,false),account_id});
            if (const auto* forwards = input.if_contains("forwards")) {
                if (!forwards->is_array() || forwards->as_array().size()>32) invalid("Forwards must be an array of at most 32 addresses");
                std::set<std::string> destinations;
                const auto address = db.query("SELECT id FROM passwd WHERE passwd_id=?",{account_id})[0][0];
                for (const auto& value : forwards->as_array()) {
                    if (!value.is_string()) invalid("Forward destination must be an address");
                    const auto destination = mailbox_address(std::string{value.as_string()});
                    if (destination == address) invalid("Forwarding an account to itself is not allowed");
                    destinations.insert(destination);
                }
                db.query("DELETE FROM forward WHERE passwd_id=?",{account_id});
                for (const auto& destination : destinations) db.query("INSERT INTO forward(passwd_id,destination) VALUES(?,?)",{account_id,destination});
            }
            policy(db,"passwd",account_id,input);
            output["account"] = std::move(accounts(db,account_id)[0]);
        }
    } else if (resource == "aliases") {
        if (method_type == Method::GET) {
            boost::json::array items;
            for (const auto& row : db.query("SELECT alias_id,mailaddr,passwd_id,organization_id FROM alias" + (id.empty() ? std::string{} : " WHERE alias_id=?") + " ORDER BY alias_id",
                    id.empty() ? Parameters{} : Parameters{id}))
                items.emplace_back(Object{{"id",std::stoll(row[0])},{"address",row[1]},{"account_id",std::stoll(row[2])},{"organization_id",std::stoll(row[3])}});
            if (!id.empty() && items.empty()) missing();
            if (id.empty()) output["aliases"] = std::move(items);
            else output["alias"] = std::move(items[0]);
        } else if (method_type == Method::DELETE) {
            method(!id.empty());
            const auto previous = db.query("SELECT mailaddr FROM alias WHERE alias_id=?",{id});
            if (previous.empty()) missing();
            retire_address(db,previous[0][0]);
            db.query("DELETE FROM alias WHERE alias_id=?",{id});
            output["deleted"] = true;
        } else {
            method((method_type == Method::POST && id.empty()) || (method_type == Method::PATCH && !id.empty()));
            fields(input,{"address","account_id"});
            auto rows = id.empty() ? Rows{} : db.query("SELECT mailaddr,passwd_id FROM alias WHERE alias_id=?",{id});
            if (!id.empty() && rows.empty()) missing();
            const auto address = input.contains("address") ? mailbox_address(text(input,"address",128)) : id.empty() ? std::string{} : rows[0][0];
            if (address.empty() || (id.empty() && !input.contains("account_id"))) invalid("Alias address and account_id are required");
            const auto account_id = input.contains("account_id") ? std::to_string(number(input.at("account_id"))) : rows[0][1];
            exists(db,"passwd",account_id);
            const auto owner = owner_of(db,address);
            if (db.query("SELECT organization_id FROM passwd WHERE passwd_id=?",{account_id})[0][0] != owner)
                throw ApiError{409,"alias_owner","An alias must belong to its target account's organization"};
            if (id.empty() || rows[0][0] != address) unused_address(db,address);
            std::string alias_id = id;
            if (id.empty()) {
                db.query("INSERT INTO alias(mailaddr,passwd_id,organization_id) VALUES(?,?,?)",{address,account_id,owner});
                alias_id=db.query("SELECT LAST_INSERT_ID()")[0][0];
                result.status=201;
            } else {
                if (rows[0][0] != address) retire_address(db,rows[0][0]);
                db.query("UPDATE alias SET mailaddr=?,passwd_id=?,organization_id=? WHERE alias_id=?",{address,account_id,owner,id});
            }
            output["alias"] = Object{{"id",std::stoll(alias_id)},{"address",address},{"account_id",std::stoll(account_id)},{"organization_id",std::stoll(owner)}};
        }
    } else missing();
    if (changed) {
        if (!db.query("SELECT p.passwd_id FROM passwd p JOIN organization o ON o.organization_id=p.organization_id WHERE COALESCE(p.spamfld,o.spamfld)=COALESCE(p.spamtrash,o.spamtrash) LIMIT 1").empty())
            throw ApiError{409,"spam_policy_conflict","Spam and trash folders must differ"};
        dirty(db);
        // Logical mutations are committed now; activation is a durable job.
        output["activation"] = activation(db);
    } else if (resource == "operations") output["activation"] = activation(db);
    transaction.commit();
    if (changed) MA_INFO << "Administrative mutation committed; resource=" << resource;
    result.body=std::move(output);
    co_return result;
}
}
