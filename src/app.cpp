#include <carafe/app.hpp>
#include <carafe/config.hpp>

#include "net/listener.hpp"
#include "server/connection.hpp"
#include "server/router.hpp"
#include "server/serve.hpp"

#include <cerrno>
#include <cstdint>
#include <memory>
#include <string_view>
#include <utility>

namespace carafe {

namespace {

constexpr bool valid_pool_limits(const PoolLimits& limits) {
    return limits.workers > 0 && limits.queued > 0 && limits.queue_wait > std::chrono::milliseconds::zero();
}

constexpr bool valid_deadlines(const Deadlines& deadlines) {
    return deadlines.idle > std::chrono::milliseconds::zero() && deadlines.request > std::chrono::milliseconds::zero();
}

}  // namespace

App::App() : router_(std::make_shared<server::Router>()) {}

void App::get(std::string_view path, http::Handler handler) {
    router_->add(http::Method::Get, path, std::move(handler));
}

void App::post(std::string_view path, http::Handler handler) {
    router_->add(http::Method::Post, path, std::move(handler));
}

void App::put(std::string_view path, http::Handler handler) {
    router_->add(http::Method::Put, path, std::move(handler));
}

void App::patch(std::string_view path, http::Handler handler) {
    router_->add(http::Method::Patch, path, std::move(handler));
}

void App::del(std::string_view path, http::Handler handler) {
    router_->add(http::Method::Delete, path, std::move(handler));
}

bool App::route(http::Method method, std::string_view path, http::Handler handler) {
    // No default: a new method has to be classified, not fall through to registrable.
    switch (method) {
        case http::Method::Head:
        case http::Method::Connect:
            return false;
        case http::Method::Get:
        case http::Method::Post:
        case http::Method::Put:
        case http::Method::Patch:
        case http::Method::Delete:
        case http::Method::Options:
        case http::Method::Trace:
            break;
    }

    router_->add(method, path, std::move(handler));
    return true;
}

std::string_view describe(RunError error) {
    // No default: a new failure has to be given words, not fall through to someone else's.
    switch (error) {
        case RunError::InvalidLimits:
            return "the pool limits would serve nobody";
        case RunError::InvalidDeadlines:
            return "a deadline of zero is no deadline at all";
        case RunError::BindFailed:
            return "the port could not be bound";
        case RunError::AcceptFailed:
            return "accepting connections stopped";
    }
    return "unknown failure";
}

RunError App::run(std::uint16_t port, PoolLimits limits, Deadlines deadlines) {
    if (!valid_pool_limits(limits)) {
        return RunError::InvalidLimits;
    }
    if (!valid_deadlines(deadlines)) {
        return RunError::InvalidDeadlines;
    }

    // The optional, not the result: both say the same thing, but only this spelling proves to the analyser that the
    // deref below is safe.
    net::ListenResult listen_result = net::listen_on(port);
    if (!listen_result.listener.has_value()) {
        return RunError::BindFailed;
    }

    net::Listener& listener = *listen_result.listener;

    server::serve_forever(listener, router_, limits, deadlines);

    return RunError::AcceptFailed;
}

}  // namespace carafe
