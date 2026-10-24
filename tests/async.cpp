#include "Async.h"
#include <boost/asio/io_context.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <chrono>
#include <iostream>
#include <stdexcept>

namespace net = boost::asio;
namespace {
net::awaitable<int> delayed_result(bool fail) {
    auto executor = co_await net::this_coro::executor;
    net::steady_timer timer{executor, std::chrono::milliseconds{5}};
    co_await timer.async_wait(net::use_awaitable);
    if (fail) throw std::runtime_error{"expected async failure"};
    co_return 42;
}
void require(bool value) {
    if (!value) throw std::runtime_error{"Coroutine bridge regression failed"};
}
}
int main() {
    net::io_context context;
    bool interleaved = false;
    bool completed = false;
    std::exception_ptr failure;
    net::spawn(context, [&](net::yield_context yield) {
        net::post(context, [&] { interleaved = true; });
        require(mailadmin::await_from_handler(yield, delayed_result(false)) == 42);
        require(interleaved);
        bool propagated = false;
        try { mailadmin::await_from_handler(yield, delayed_result(true)); }
        catch (const std::runtime_error& error) {
            propagated = std::string_view{error.what()} == "expected async failure";
        }
        require(propagated);
        completed = true;
    }, [&](std::exception_ptr error) { failure = error; });
    context.run();
    if (failure || !completed) {
        std::cerr << "Coroutine bridge regression failed\n";
        return 1;
    }
    std::cout << "Coroutine bridge success, failure and suspension checks passed\n";
}
