#pragma once
#include "Config.h"
#include <boost/asio/awaitable.hpp>
#include <boost/json.hpp>
#include <cstdint>
#include <map>
#include <memory>
#include <stdexcept>

namespace mailadmin {
struct ApiError : std::runtime_error {
    int status;
    std::string code;
    ApiError(int status, std::string code, std::string message)
        : std::runtime_error{std::move(message)}, status{status}, code{std::move(code)} {}
};
struct Domain {
    std::int64_t organization_id = 0;
    bool hosted = false;
    std::uint64_t accounts = 0;
    std::uint64_t aliases = 0;
};
struct Snapshot {
    boost::json::array organizations;
    std::map<std::string, Domain> domains;
    boost::json::array domain_json() const;
    std::string metrics() const;
};
// Runs exclusively on the bounded database worker pool, not HTTP executors.
boost::asio::awaitable<Snapshot> read_inventory(std::shared_ptr<const Config> config);
}
