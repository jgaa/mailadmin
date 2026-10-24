#pragma once
#include "Database.h"
#include <mysql.h>
#include <optional>
#include <vector>

namespace mailadmin {
using Rows = std::vector<std::vector<std::string>>;
using Parameters = std::vector<std::optional<std::string>>;
class Connection {
public:
    explicit Connection(const Config& config);
    ~Connection();
    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;
    Rows query(std::string_view sql, const Parameters& parameters = {});
private:
    void close() noexcept;
    MYSQL* mysql_ = nullptr;
};
class Transaction {
public:
    explicit Transaction(Connection& connection) : connection_{connection} { connection_.query("START TRANSACTION"); }
    ~Transaction() { if (!committed_) { try { connection_.query("ROLLBACK"); } catch (...) {} } }
    void commit() { connection_.query("COMMIT"); committed_ = true; }
private:
    Connection& connection_;
    bool committed_ = false;
};
// Serialize administrative writers/reconciliation across processes, including
// CLI operations. InnoDB transactions still provide atomic durable DB changes.
class AdministrationLock {
public:
    explicit AdministrationLock(Connection& connection) : connection_{connection} {
        if (connection_.query("SELECT GET_LOCK('mailadmin.administration',2)").at(0).at(0) != "1")
            throw ApiError{503, "busy", "Another administrative operation is running"};
    }
    ~AdministrationLock() { try { connection_.query("SELECT RELEASE_LOCK('mailadmin.administration')"); } catch (...) {} }
private:
    Connection& connection_;
};
std::int64_t identifier(std::string_view value);
std::string domain_name(std::string value);
std::string address_domain(const std::string& value);
}
