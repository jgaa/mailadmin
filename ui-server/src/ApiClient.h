#pragma once

#include "Data.h"
#include "Credentials.h"
#include <boost/asio.hpp>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <variant>

namespace restincurl { class Client; }

namespace mailadmin::ui {

using Payload = std::variant<Inventory, Mailbox, Document, Mutation>;
struct Outcome {
    std::optional<Payload> payload;
    std::string error;
    bool unauthorized = false;
    bool uncertain = false;
};
// Jobs hold no application/widget pointers. Expired sessions can disappear
// safely while their bounded network requests finish in the background.
struct Pending {
    std::mutex mutex;
    std::optional<Outcome> outcome;
};

class ApiClient {
public:
    explicit ApiClient(std::string base_url);
    ~ApiClient();
    ApiClient(const ApiClient&) = delete;
    ApiClient& operator=(const ApiClient&) = delete;
    std::shared_ptr<Pending> inventory(std::shared_ptr<const Credential> credential);
    std::shared_ptr<Pending> mailbox(std::shared_ptr<const Credential> credential, std::int64_t id);
    std::shared_ptr<Pending> document(std::shared_ptr<const Credential> credential, std::string resource, std::string id);
    std::shared_ptr<Pending> mutate(std::shared_ptr<const Credential> credential, std::string method, std::string resource, std::string id, Object input = {});
private:
    boost::asio::awaitable<Value> request(std::shared_ptr<const Credential> credential, std::string method, std::string path, Object input = {});
    boost::asio::awaitable<Value> get(std::shared_ptr<const Credential> credential, std::string path);
    boost::asio::awaitable<Payload> load_inventory(std::shared_ptr<const Credential> credential);
    boost::asio::awaitable<Payload> load_mailbox(std::shared_ptr<const Credential> credential, std::int64_t id);
    boost::asio::awaitable<Payload> load_document(std::shared_ptr<const Credential> credential, std::string resource, std::string id);
    boost::asio::awaitable<Payload> change(std::shared_ptr<const Credential> credential, std::string method, std::string resource, std::string id, Object input);
    std::shared_ptr<Pending> start(boost::asio::awaitable<Payload> operation);

    std::string base_url_;
    std::unique_ptr<restincurl::Client> client_;
    boost::asio::io_context io_;
    boost::asio::executor_work_guard<boost::asio::io_context::executor_type> work_;
    std::thread worker_;
};

}
