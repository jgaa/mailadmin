#pragma once
#include "Sql.h"
#include <boost/asio.hpp>
#include <atomic>
#include <memory>
#include <thread>

namespace restincurl { class Client; }
namespace mailadmin {
void validate_dns_config(const Config& config);
void validate_dns_desired(const Config& config, const boost::json::object& desired);
boost::json::object dns_defaults(const Config& config);

// Hold each named lock through the surrounding transaction's commit. The DNS
// worker uses the same lock throughout one bounded exchange. API writers fail
// promptly rather than queueing a newer revision behind an old request.
class DnsLocks {
public:
    explicit DnsLocks(Connection& db) : db_{db} {}
    ~DnsLocks();
    void lock(const std::string& domain);
private:
    Connection& db_;
    std::vector<std::string> locks_;
};
void migrate_dns(Connection& db);
void dns_available_name(Connection& db, const std::string& domain);
void queue_dns(Connection& db, DnsLocks& locks, const Config& config, const std::string& domain,
    const std::string& method, const boost::json::object& desired = {});
void dns_created(Connection& db, DnsLocks& locks, const Config& config, const std::string& domain, const boost::json::object& input);
void dns_removed(Connection& db, DnsLocks& locks, const Config& config, const std::string& domain);
boost::json::object dns_state(Connection& db, const Config& config, const std::string& domain);
boost::json::array dns_jobs(Connection& db);

class DnsWorker {
public:
    explicit DnsWorker(std::shared_ptr<const Config> config);
    ~DnsWorker();
    void stop();
private:
    boost::asio::awaitable<void> run();
    boost::asio::awaitable<void> step();
    std::shared_ptr<const Config> config_;
    std::string key_;
    std::unique_ptr<restincurl::Client> client_;
    boost::asio::io_context io_;
    boost::asio::steady_timer timer_;
    std::atomic<bool> stopping_{false};
    std::thread thread_;
};
}
