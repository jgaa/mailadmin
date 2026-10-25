#pragma once
#include "Database.h"
#include "Sql.h"
#include <condition_variable>
#include <thread>

namespace mailadmin {
class DnsWorker;
struct Operation {
    int status = 200;
    boost::json::value body;
};
void migrate_schema(const Config& config);
void migrate_passwords(const Config& config);
boost::asio::awaitable<Operation> administer(std::shared_ptr<const Config> config,
    yahat::Request::Type method, std::string target, std::string body);
void reconcile(const Config& config, bool clean_spam);
class Reconciler {
public:
    explicit Reconciler(std::shared_ptr<const Config> config);
    ~Reconciler();
    void stop();
private:
    std::shared_ptr<const Config> config_;
    std::mutex mutex_;
    std::condition_variable condition_;
    bool stopping_ = false;
    std::thread thread_;
    std::unique_ptr<DnsWorker> dns_;
};
void quota_value(std::string_view quota);
std::string mailbox_address(std::string value);
std::string folder_name(std::string value, bool allow_empty = false);
}
