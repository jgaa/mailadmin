#include "Api.h"
#include "Async.h"
#include "Database.h"
#include "logging.h"
#include "mailadmin/version.h"
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/uuid/uuid_io.hpp>

namespace mailadmin {
namespace {
yahat::Response json_response(int status, boost::json::value body, const std::string& request_id) {
    yahat::Response response{status, status < 400 ? "OK" : "Request failed", boost::json::serialize(body)};
    response.mime_type = "application/json";
    response.headers.set("Cache-Control", "no-store");
    response.headers.set("X-Request-ID", request_id);
    return response;
}
yahat::Response failure(int status, std::string code, std::string message, const std::string& request_id) {
    MA_WARN << "API request failed: id=" << request_id << " status=" << status << " code=" << code;
    auto response = json_response(status, boost::json::object{{"error", boost::json::object{
        {"code", std::move(code)}, {"message", std::move(message)}, {"request_id", request_id}}}}, request_id);
    if (status == 401) response.headers.set("WWW-Authenticate", "Bearer");
    if (status == 405) response.headers.set("Allow", "GET, POST, PUT, PATCH, DELETE");
    return response;
}

class Handler final : public yahat::RequestHandler {
public:
    explicit Handler(std::shared_ptr<Service> service) : service_{std::move(service)} {}
    yahat::Response onReqest(const yahat::Request& request) override {
        if (!request.yield) return api_error(request, 500, "internal_error", "Missing HTTP coroutine");
        auto yield = *request.yield;
        // Named coroutine; service and inputs remain owned until it completes.
        return await_from_handler(yield,
            dispatch(service_, request.target, boost::uuids::to_string(request.uuid), request.type, request.body));
    }
private:
    static boost::asio::awaitable<yahat::Response> dispatch(std::shared_ptr<Service> service,
            std::string target, std::string request_id, yahat::Request::Type method, std::string body) {
        co_return co_await service->respond(std::move(target), std::move(request_id), method, std::move(body));
    }
    std::shared_ptr<Service> service_;
};
}
Service::Service(std::shared_ptr<const Config> config)
    : config_{std::move(config)}, workers_{config_->database_workers}, reconciler_{config_} {}
void Service::finish_database_work() {
    reconciler_.stop();
    workers_.join();
}

boost::asio::awaitable<yahat::Response> Service::respond(std::string target, std::string request_id, yahat::Request::Type method, std::string body) {
    // Atomics bound queued + executing DB operations across every route/session.
    if (pending_.fetch_add(1) >= config_->max_pending_requests) {
        pending_.fetch_sub(1);
        co_return failure(503, "busy", "Too many pending operations", request_id);
    }
    struct Pending {
        std::atomic<unsigned>& counter;
        ~Pending() { counter.fetch_sub(1); }
    } pending{pending_};
    try {
        const bool inventory_route = target == "/api/v1/health" || target == "/api/v1/organizations"
            || target == "/api/v1/domains" || target == "/metrics";
        if ((target == "/api/v1/health" || target == "/metrics") && method != yahat::Request::Type::GET)
            co_return failure(405,"method_not_allowed","This endpoint only accepts GET",request_id);
        if (method != yahat::Request::Type::GET || !inventory_route) {
            if (!config_->administration) {
                if (inventory_route) co_return failure(405,"method_not_allowed","Only GET is supported in inventory mode",request_id);
                co_return failure(404,"not_found","Endpoint not found",request_id);
            }
            auto operation = co_await boost::asio::co_spawn(workers_.get_executor(),
                administer(config_,method,std::move(target),std::move(body)),boost::asio::use_awaitable);
            co_return json_response(operation.status,std::move(operation.body),request_id);
        }
        if (!body.empty()) throw ApiError{400,"unexpected_body","GET requests do not accept a body"};
        auto snapshot = co_await boost::asio::co_spawn(workers_.get_executor(),
            read_inventory(config_), boost::asio::use_awaitable);
        MA_TRACE << "Inventory operation completed: id=" << request_id;
        if (target == "/metrics") {
            yahat::Response response{200, "OK", snapshot.metrics()};
            response.mime_type = "application/openmetrics-text; version=1.0.0; charset=utf-8";
            response.headers.set("Cache-Control", "no-store");
            response.headers.set("X-Request-ID", request_id);
            co_return response;
        }
        if (target == "/api/v1/health") co_return json_response(200,
            boost::json::object{{"status", "ready"}, {"version", MAILADMIN_VERSION}, {"mode", config_->administration ? "administration" : "inventory"}}, request_id);
        if (target == "/api/v1/organizations") co_return json_response(200,
            boost::json::object{{"organizations", std::move(snapshot.organizations)}}, request_id);
        co_return json_response(200, boost::json::object{{"domains", snapshot.domain_json()}}, request_id);
    } catch (const ApiError& error) {
        co_return failure(error.status, error.code, error.what(), request_id);
    } catch (const std::exception&) {
        // Unknown exception text may contain connector/input data: never log it.
        MA_ERROR << "Unexpected API failure: id=" << request_id;
        co_return failure(500, "internal_error", "Internal server error", request_id);
    }
}
yahat::Response api_error(const yahat::Request& request, int status, std::string code, std::string message) {
    return failure(status, std::move(code), std::move(message), boost::uuids::to_string(request.uuid));
}
void mount_api(yahat::HttpServer& server, std::shared_ptr<Service> service) {
    yahat::RouteOptions options;
    options.auto_handle_cors = false;
    options.allowed_origins = std::vector<std::string>{};
    options.unauthorized_response = [](const yahat::Request& request) {
        return api_error(request, 401, "unauthorized", "A valid bearer API key is required");
    };
    // Authentication precedes this handler. Expected errors use one JSON/log
    // contract, including unknown paths and unsupported methods.
    options.request_filter = [](const yahat::Request& request) -> std::optional<yahat::Response> {
        if (!request.body.empty() && request.type != yahat::Request::Type::GET && request.type != yahat::Request::Type::DELETE) {
            const auto content_type = request.headers[boost::beast::http::field::content_type];
            if (content_type != "application/json" && content_type != "application/json; charset=utf-8")
                return api_error(request,415,"unsupported_media_type","Use Content-Type: application/json");
        }
        return std::nullopt;
    };
    auto handler = std::make_shared<Handler>(std::move(service));
    // Yahat prefixes match at path boundaries. A '/' route alone does not
    // match '/api/...'; explicitly register each application prefix.
    server.addRouteWithOptions("/api/v1", handler, options);
    options.exact_match = true;
    server.addRouteWithOptions("/metrics", handler, options);
    server.addRouteWithOptions("/", handler, options);
}
}
