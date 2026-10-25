#include "ApiClient.h"

// Do not enable RESTinCurl's request/transport logging: credentials and API
// response bodies must never reach a log, even at trace level.
#define RESTINCURL_LOG(message) do {} while (false)
#define RESTINCURL_LOG_TRACE(message) do {} while (false)
#include <restincurl/restincurl.h>
#include "logging.h"

#include <openssl/crypto.h>
#include <algorithm>
#include <cctype>
#include <limits>
#include <stdexcept>
#include <string_view>

namespace mailadmin::ui {
namespace {
constexpr std::size_t max_body = 32 * 1024 * 1024;
struct Failure : std::runtime_error {
    explicit Failure(std::string message, bool unauthorized = false, bool uncertain = false)
        : std::runtime_error{std::move(message)}, unauthorized{unauthorized}, uncertain{uncertain} {}
    bool unauthorized;
    bool uncertain;
};
std::size_t receive(char* bytes, std::size_t size, std::size_t count, void* context) noexcept {
    auto& body = *static_cast<std::string*>(context);
    if (size != 0 && count > std::numeric_limits<std::size_t>::max() / size) return 0;
    const auto length = size * count;
    if (length > max_body - body.size()) return 0;
    try { body.append(bytes, length); } catch (...) { return 0; }
    return length;
}

void validate_url(const std::string& url) {
    std::unique_ptr<CURLU, decltype(&curl_url_cleanup)> parsed{curl_url(), curl_url_cleanup};
    if (!parsed || curl_url_set(parsed.get(), CURLUPART_URL, url.c_str(), 0) != CURLUE_OK)
        throw std::runtime_error{"Invalid API URL"};
    auto part = [&](CURLUPart field) {
        char* raw = nullptr;
        (void)curl_url_get(parsed.get(), field, &raw, 0);
        std::unique_ptr<char, decltype(&curl_free)> memory{raw, curl_free};
        return raw ? std::string{raw} : std::string{};
    };
    const auto scheme = part(CURLUPART_SCHEME);
    const auto host = part(CURLUPART_HOST);
    if ((scheme != "https" && !(scheme == "http" && (host == "127.0.0.1" || host == "[::1]")))
        || !part(CURLUPART_USER).empty() || !part(CURLUPART_PASSWORD).empty()
        || !part(CURLUPART_QUERY).empty() || !part(CURLUPART_FRAGMENT).empty()
        || part(CURLUPART_PATH) != "/")
        throw std::runtime_error{"API URL must be an HTTPS origin or numeric loopback HTTP origin"};
}
}

ApiClient::ApiClient(std::string base_url)
    : base_url_{std::move(base_url)}, client_{std::make_unique<restincurl::Client>()}, work_{boost::asio::make_work_guard(io_)} {
    validate_url(base_url_);
    while (base_url_.ends_with('/')) base_url_.pop_back();
    worker_ = std::thread{[this] { io_.run(); }};
}
ApiClient::~ApiClient() {
    // Drain the bounded transfers and their coroutine completions before the
    // REST client or executor is destroyed. Shutdown never strands handlers.
    work_.reset();
    if (worker_.joinable()) worker_.join();
}

boost::asio::awaitable<Value> ApiClient::get(std::shared_ptr<const Credential> credential, std::string path) {
    co_return co_await request(std::move(credential), "GET", std::move(path));
}
boost::asio::awaitable<Value> ApiClient::request(std::shared_ptr<const Credential> credential, std::string method, std::string path, Object input) {
    UI_DEBUG << "Fetching API resource " << path;
    std::string body;
    auto wire = method == "GET" || method == "DELETE" ? std::string{} : boost::json::serialize(input);
    struct Wipe {
        Object& input; std::string& wire;
        ~Wipe() {
            if (auto* password = input.if_contains("password"); password && password->is_string())
                OPENSSL_cleanse(password->as_string().data(), password->as_string().size());
            OPENSSL_cleanse(wire.data(), wire.size());
        }
    } wipe{input, wire};
    auto request = client_->Build();
    if (method == "POST") request->Post(base_url_ + path);
    else if (method == "PATCH") request->Patch(base_url_ + path);
    else if (method == "PUT") request->Put(base_url_ + path);
    else if (method == "DELETE") request->Delete(base_url_ + path);
    else request->Get(base_url_ + path);
    if (!wire.empty()) request->Header("Content-Type", "application/json").SendData(wire);
    request->Header("Authorization", "Bearer " + credential->value()).AcceptJson()
        .ConnectTimeout(3000).RequestTimeout(10000)
        .Option(CURLOPT_FOLLOWLOCATION, 0L)
        .Option(CURLOPT_PROTOCOLS_STR, "http,https")
        .Option(CURLOPT_PROXY, "")
        .Option(CURLOPT_SSL_VERIFYPEER, 1L).Option(CURLOPT_SSL_VERIFYHOST, 2L)
        .Option(CURLOPT_NOSIGNAL, 1L)
        .SetWriteHandler(receive, &body);
    const auto reply = co_await request->AsioAsyncExecute(boost::asio::use_awaitable);
    if (reply.curl_code != CURLE_OK) {
        UI_WARN << "API transport failed; curl code=" << static_cast<int>(reply.curl_code);
        throw Failure{method == "GET" ? "API connection failed. Check the API service." : "The save outcome is unknown. Reload the record before trying again; do not repeat a password-generation request blindly.", false, method != "GET"};
    }
    if (reply.http_response_code != 200 && reply.http_response_code != 201) {
        const auto status = reply.http_response_code;
        UI_WARN << "API request failed; HTTP status=" << status;
        // Do not forward arbitrary backend response bodies to a page or log.
        if (status == 401) throw Failure{"The UI server's configured API key was rejected. Ask the sysop to update its credential and restart the UI service.", true};
        if (status == 403) throw Failure{"API access denied. Check the API's authentication and network policy."};
        if (status == 413) throw Failure{"The form exceeds the API's size limit. Shorten the auto-reply, destination list or DNS configuration."};
        if (status == 400 || status == 409) {
            boost::system::error_code error;
            const auto value = boost::json::parse(body, error);
            if (!error && value.is_object()) {
                if (const auto* api_error = value.as_object().if_contains("error"); api_error && api_error->is_object()) {
                    if (const auto* message = api_error->as_object().if_contains("message"); message && message->is_string() && message->as_string().size() <= 512)
                        throw Failure{std::string{message->as_string()}};
                }
            }
            throw Failure{"Invalid input or a conflicting record. Review the form and try again."};
        }
        if (status == 404 || status == 405) throw Failure{"Resource unavailable. Enable API administration mode or refresh after a deletion."};
        throw Failure{"API request failed (HTTP " + std::to_string(status) + "). Reload the record before repeating a save.", false, method != "GET" && status >= 500};
    }
    boost::system::error_code error;
    auto json = boost::json::parse(body, error);
    if (error || !json.is_object()) throw Failure{"The API returned an invalid JSON response. Reload before repeating a save.", false, method != "GET"};
    UI_TRACE << "API resource decoded; bytes=" << body.size();
    co_return json;
}

boost::asio::awaitable<Payload> ApiClient::load_inventory(std::shared_ptr<const Credential> credential) {
    const auto organizations = co_await get(credential, "/api/v1/organizations");
    const auto domains = co_await get(credential, "/api/v1/domains");
    const auto accounts = co_await get(credential, "/api/v1/accounts");
    co_return parse_inventory(organizations, domains, accounts);
}
boost::asio::awaitable<Payload> ApiClient::load_mailbox(std::shared_ptr<const Credential> credential, std::int64_t id) {
    const auto account = co_await get(credential, "/api/v1/accounts/" + std::to_string(id));
    if (record_id(account.at("account").at("id")) != id) throw Failure{"API returned a different mailbox; refresh the inventory"};
    const auto owner = record_id(account.at("account").at("organization_id"));
    const auto organization = co_await get(credential, "/api/v1/organizations/" + std::to_string(owner));
    const auto aliases = co_await get(credential, "/api/v1/aliases");
    co_return parse_mailbox(account, organization, aliases);
}

std::shared_ptr<Pending> ApiClient::start(boost::asio::awaitable<Payload> operation) {
    auto pending = std::make_shared<Pending>();
    boost::asio::co_spawn(io_, std::move(operation), [pending](std::exception_ptr failure, Payload payload) {
        Outcome outcome;
        if (failure) {
            try { std::rethrow_exception(failure); }
            catch (const Failure& error) { outcome.error = error.what(); outcome.unauthorized = error.unauthorized; outcome.uncertain = error.uncertain; }
            catch (...) {
                UI_ERROR << "API response could not be decoded";
                outcome.error = "API data could not be loaded. Refresh the inventory and check the server logs.";
            }
        } else outcome.payload = std::move(payload);
        std::lock_guard lock{pending->mutex};
        pending->outcome = std::move(outcome);
    });
    return pending;
}
std::shared_ptr<Pending> ApiClient::inventory(std::shared_ptr<const Credential> credential) {
    return start(load_inventory(std::move(credential)));
}
std::shared_ptr<Pending> ApiClient::mailbox(std::shared_ptr<const Credential> credential, std::int64_t id) {
    if (id <= 0 || id > 2147483647) throw std::runtime_error{"Invalid mailbox identifier"};
    return start(load_mailbox(std::move(credential), id));
}

namespace {
std::string resource_path(const std::string& resource, const std::string& id) {
    if (resource != "organizations" && resource != "domains" && resource != "accounts" && resource != "aliases" && resource != "dns" && resource != "operations")
        throw std::runtime_error{"Unknown API resource"};
    for (const unsigned char c : id)
        if (!(std::isalnum(c) || c == '.' || c == '-')) throw std::runtime_error{"Invalid record identifier"};
    return "/api/v1/" + (resource == "dns" ? "domains/" + id + "/dns" : resource + (id.empty() ? "" : "/" + id));
}
}
boost::asio::awaitable<Payload> ApiClient::load_document(std::shared_ptr<const Credential> credential, std::string resource, std::string id) {
    auto value = co_await get(std::move(credential), resource_path(resource, id));
    co_return Document{std::move(resource), std::move(value.as_object())};
}
boost::asio::awaitable<Payload> ApiClient::change(std::shared_ptr<const Credential> credential, std::string method, std::string resource, std::string id, Object input) {
    auto value = co_await request(std::move(credential), std::move(method), resource_path(resource, id), std::move(input));
    co_return Mutation{std::move(value.as_object())};
}
std::shared_ptr<Pending> ApiClient::document(std::shared_ptr<const Credential> credential, std::string resource, std::string id) {
    return start(load_document(std::move(credential), std::move(resource), std::move(id)));
}
std::shared_ptr<Pending> ApiClient::mutate(std::shared_ptr<const Credential> credential, std::string method, std::string resource, std::string id, Object input) {
    if (method != "POST" && method != "PATCH" && method != "PUT" && method != "DELETE") throw std::runtime_error{"Invalid mutation method"};
    return start(change(std::move(credential), std::move(method), std::move(resource), std::move(id), std::move(input)));
}

}
