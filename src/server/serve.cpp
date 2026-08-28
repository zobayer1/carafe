#include "server/serve.hpp"

#include <carafe/http/request.hpp>
#include <carafe/http/response.hpp>

#include "http/ascii.hpp"
#include "http/field_list.hpp"
#include "http/request_reader.hpp"
#include "server/connection.hpp"
#include "server/router.hpp"

#include <chrono>
#include <cstddef>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace carafe::server {

// Long enough that an exhausted server is not spinning, short enough that a descriptor freed a moment later is not left
// waiting on the clock.
constexpr auto accept_retry_pause = std::chrono::milliseconds(10);

namespace {

// No default label, so a new enumerator breaks this build rather than becoming a silent 400.
int status_for(http::RequestError error) {
    switch (error) {
        case http::RequestError::UnknownMethod:
        case http::RequestError::UnsupportedTransferEncoding:
            return 501;
        case http::RequestError::UnsupportedVersion:
            return 505;
        case http::RequestError::BodyTooLarge:
            return 413;
        case http::RequestError::RequestLineTooLong:
            return 414;
        case http::RequestError::HeaderTooLong:
        case http::RequestError::TooManyHeaders:
        case http::RequestError::HeadTooLarge:
            return 431;
        case http::RequestError::Malformed:
        case http::RequestError::None:
            break;
    }
    return 400;
}

// RFC 9110 §7.6.1: a comma-separated list of case-insensitive connection options. §5.3 folds a repeated field into one
// list, so every connection field is scanned and not just the first. `option` must already be lowercase.
[[nodiscard]] bool has_connection_option(const http::Headers& headers, std::string_view option) noexcept {
    for (const auto& header : headers) {
        // Stored lowercased by Headers::add, so a plain compare is enough.
        if (header.name != "connection") {
            continue;
        }
        const std::string_view value = header.value;
        std::size_t start = 0;
        while (const auto token = http::next_list_element(value, start)) {
            if (http::ascii_equals_lowered(option, *token)) {
                return true;
            }
        }
    }
    return false;
}

// RFC 9112 §9.3: persistent by default on HTTP/1.1, and not on HTTP/1.0 unless the client asks. "close" is definitive
// either way, so it is tested first.
[[nodiscard]] bool client_wants_close(const http::Request& request) noexcept {
    if (has_connection_option(request.headers, "close")) {
        return true;
    }

    // No default: a new version states its own persistence rather than inheriting 1.1's.
    switch (request.version) {
        case http::Version::Http10:
            return !has_connection_option(request.headers, "keep-alive");
        case http::Version::Http11:
            break;
    }
    return false;
}

// Sends one response and reports whether the connection outlives it. The close is announced before it is performed,
// RFC 9112 §9.6, or a client cannot tell a deliberate end from a truncated reply.
[[nodiscard]] bool answer(Connection& conn, http::Response response, bool closing, bool with_body) {
    if (closing) {
        response.headers.add({"connection", "close"});
    }
    if (!conn.write(response.serialize(with_body))) {
        return false;  // nobody left to answer
    }
    return !closing;
}

// The body names the status: a bare 404 tells a terminal reader nothing.
http::Response status_response(int status) {
    std::string body = std::to_string(status);
    body += ' ';
    body += http::status_message(status);
    body += '\n';
    return http::text_response(status, std::move(body));
}

// Comma-separated, as RFC 9110 spells the field. Method names are case-sensitive tokens, so these stay uppercase though
// every field name we emit is lowered.
std::string allow_value(const std::vector<http::Method>& methods) {
    std::string value;
    for (const http::Method method : methods) {
        if (!value.empty()) {
            value += ", ";
        }
        value += http::method_name(method);
    }
    return value;
}

// A path nobody registered is a 404; one registered under another method is a 405, and RFC 9110 makes Allow on that 405
// a MUST rather than a courtesy.
http::Response unmatched_response(const Router& router, std::string_view target, bool path_matched) {
    if (!path_matched) {
        return status_response(404);
    }
    http::Response response = status_response(405);
    response.headers.add({"allow", allow_value(router.allowed_methods(target))});
    return response;
}

enum class AcceptRetry { Never, Immediately, AfterAPause };

// Only the listener failing may end the server. A failure of the connection being accepted belongs to one client, and
// exhaustion belongs to the moment: both leave a socket that still accepts.
//
// A list rather than a fallthrough, because errno is not an enum and nothing makes a new value announce itself.
// Guessing "transient" for one that is not trades a server that stopped for a server that spins.
[[nodiscard]] AcceptRetry accept_retry_for(int os_error) noexcept {
    switch (os_error) {
        // Given back as connections finish, so the wait is short and asking again at once only spends a core.
        case EMFILE:
        case ENFILE:
        case ENOMEM:
        case ENOBUFS:
            return AcceptRetry::AfterAPause;

        // The connection, not the socket that accepted it: a firewall refusing this one, a client gone from the queue,
        // or any protocol error accept(2) says it may surface for the new socket.
        case ECONNABORTED:
        case EPERM:
        case EPROTO:
        case ETIMEDOUT:
        case ENETDOWN:
        case ENETUNREACH:
        case EHOSTDOWN:
        case EHOSTUNREACH:
            return AcceptRetry::Immediately;

        default:
            return AcceptRetry::Never;
    }
}

}  // namespace

void serve_connection(Connection& conn, const Router& router) {
    while (true) {
        auto result = conn.next_request();

        if (!result) {
            // A read failure gets no reply because nobody is listening.
            if (result.os_error != 0) {
                return;
            }

            http::Response response = status_response(status_for(result.error));

            // Two reasons to close, and a failure carries no headers to consult: a 1.0 client that did ask to stay open
            // is closed on anyway. Legal, and the other way round leaves one that did not ask waiting forever.
            if (!answer(conn, std::move(response), !result.stream_continues || result.version == http::Version::Http10,
                        true)) {
                return;
            }

            continue;
        }

        if (!result.request) {
            return;  // the client finished
        }

        http::Request& request = *result.request;
        auto match = router.find(request.method, request.target);

        // Unconditional: an unmatched request captured nothing, so this costs an empty vector rather than a branch.
        request.params = std::move(match.params);

        http::Response response =
            match ? (*match.handler)(request) : unmatched_response(router, request.target, match.path_matched);

        if (!answer(conn, std::move(response), client_wants_close(request), request.method != http::Method::Head)) {
            return;
        }
    }
}

void serve_forever(net::Listener& listener, const std::shared_ptr<const Router>& router) {
    while (true) {
        auto accepted = listener.accept();
        if (!accepted.client.has_value()) {
            switch (accept_retry_for(accepted.os_error)) {
                case AcceptRetry::Never:
                    return;
                case AcceptRetry::AfterAPause:
                    std::this_thread::sleep_for(accept_retry_pause);
                    break;
                case AcceptRetry::Immediately:
                    break;
            }
            continue;
        }

        // One thread each, so a client holding a persistent connection open cannot keep every other client waiting.
        // The router goes in by value: a detached thread may outlive the App that registered the routes.
        try {
            std::thread([client = std::move(*accepted.client), router]() mutable {
                Connection conn{std::move(client)};
                serve_connection(conn, *router);
            }).detach();
        } catch (const std::system_error&) {
            // Out of threads. The client is dropped as the socket goes down with the lambda that failed to launch:
            // serving it here would block every accept behind it, and letting this escape would take the connections
            // already in flight with it.
            continue;
        }
    }
}

}  // namespace carafe::server
