#pragma once
#include "Config.h"
#include "Security.h"
#include "Administration.h"
#include <boost/asio/thread_pool.hpp>
#include <atomic>
#include <memory>

namespace mailadmin {
class Service {
public:
    explicit Service(std::shared_ptr<const Config> config);
    boost::asio::awaitable<yahat::Response> respond(std::string target, std::string request_id, yahat::Request::Type method, std::string body);
    void finish_database_work();
private:
    std::shared_ptr<const Config> config_;
    boost::asio::thread_pool workers_;
    std::atomic<unsigned> pending_{0};
    Reconciler reconciler_;
};
void mount_api(yahat::HttpServer& server, std::shared_ptr<Service> service);
yahat::Response api_error(const yahat::Request& request, int status, std::string code, std::string message);
}
