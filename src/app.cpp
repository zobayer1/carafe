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

App::App() : router_(std::make_unique<server::Router>()) {}

App::~App() = default;

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
    // No default: a method added to the enum has to be classified here rather
    // than fall through to registrable.
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
    // The optional is tested rather than the result: both say the same thing here,
    // but only this spelling proves to the analyser that the deref below is safe.
    net::ListenResult listen_result = net::listen_on(port);
    if (!listen_result.listener.has_value()) {
        return false;
    }

    net::Listener& listener = *listen_result.listener;

    while (true) {
        auto accepted = listener.accept();
        if (!accepted.client.has_value()) {
            // A connection dying in the queue before it is taken is routine and must
            // not stop the server. Anything else means the listener itself is
            // finished, and retrying would spin hot on the same error.
            if (accepted.os_error == ECONNABORTED) {
                continue;
            }
            return false;
        }

        // Served to completion before the next accept, and closed when conn leaves
        // scope. One client at a time until the concurrency milestone.
        server::Connection conn{std::move(*accepted.client)};
        server::serve_connection(conn, *router_);
    }
}

}  // namespace carafe
