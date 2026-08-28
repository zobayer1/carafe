#include <carafe/app.hpp>

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

bool App::run(std::uint16_t port) {
    // The optional, not the result: both say the same thing, but only this spelling proves to the analyser that the
    // deref below is safe.
    net::ListenResult listen_result = net::listen_on(port);
    if (!listen_result.listener.has_value()) {
        return false;
    }

    net::Listener& listener = *listen_result.listener;

    server::serve_forever(listener, router_);

    return false;
}

}  // namespace carafe
