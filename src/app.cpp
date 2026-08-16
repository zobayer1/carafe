#include <carafe/app.hpp>

#include "net/listener.hpp"
#include "server/connection.hpp"
#include "server/serve.hpp"

#include <cerrno>
#include <cstdint>
#include <utility>

namespace carafe {

// Static today only because App holds no routes yet. Making it static now would
// have to be undone, and that breaks every caller that wrote App::run.
// NOLINTNEXTLINE(readability-convert-member-functions-to-static)
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
        server::serve_connection(conn);
    }
}

}  // namespace carafe
