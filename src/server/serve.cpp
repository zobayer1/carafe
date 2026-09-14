#include "server/serve.hpp"

#include <carafe/config.hpp>
#include <carafe/http/request.hpp>
#include <carafe/http/response.hpp>

#include "http/ascii.hpp"
#include "http/field_list.hpp"
#include "http/request_reader.hpp"
#include "server/connection.hpp"
#include "server/pipeline.hpp"
#include "server/pool.hpp"

#include <cerrno>
#include <chrono>
#include <cstddef>
#include <string_view>
#include <thread>
#include <utility>

namespace carafe::server {

// Long enough that an exhausted server is not spinning, short enough that a descriptor freed a moment later is not left
// waiting on the clock.
constexpr auto accept_retry_pause = std::chrono::milliseconds(10);

namespace {

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

// No default label, so a new enumerator breaks this build rather than becoming a silent 400.
[[nodiscard]] int status_for(http::RequestError error) noexcept {
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

// One value on Linux, and not required to be.
[[nodiscard]] bool timed_out(int os_error) noexcept {
    return os_error == EAGAIN || os_error == EWOULDBLOCK || os_error == ETIMEDOUT;
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

}  // namespace

void serve_connection(Connection& conn, const Pipeline& pipeline) {
    while (true) {
        auto result = conn.next_request();

        if (!result) {
            // A read failure gets no reply because nobody is listening.
            if (result.os_error != 0) {
                // A deadline that fired with a request half-received is a client that asked and heard nothing back.
                // Any other read failure, and an idle connection, has nobody to tell.
                if (timed_out(result.os_error) && conn.request_in_progress()) {
                    static_cast<void>(answer(conn, http::status_response(408), true, true));
                }
                return;
            }

            http::Response response = http::status_response(status_for(result.error));

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
        http::Response response = pipeline.respond(request);

        if (!answer(conn, std::move(response), client_wants_close(request), request.method != http::Method::Head)) {
            return;
        }
    }
}

void serve_forever(net::Listener& listener, const std::shared_ptr<const Pipeline>& pipeline, PoolLimits limits,
                   Deadlines deadlines) {
    ConnectionPool pool{pipeline, limits, deadlines};

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

        pool.submit(std::move(*accepted.client));
    }
}

}  // namespace carafe::server
