#pragma once
#include <utility>
#include <exception>
#include <type_traits>
#include <boost/asio/as_tuple.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/spawn.hpp>

namespace mailadmin {
// Preserve both completion arguments rather than letting older Boost spawn
// specializations rethrow a null exception_ptr on successful co_spawn results.
template<typename T> requires (!std::is_void_v<T>)
T await_from_handler(boost::asio::yield_context yield, boost::asio::awaitable<T> operation) {
    auto [failure, result] = boost::asio::co_spawn(yield.get_executor(),
        std::move(operation), boost::asio::as_tuple(yield));
    if (failure) std::rethrow_exception(failure);
    return std::move(result);
}
}
