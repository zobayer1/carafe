#include "server/serve.hpp"

#include <carafe/http/request.hpp>
#include <carafe/http/response.hpp>

#include "http/request_reader.hpp"
#include "server/connection.hpp"
#include "server/router.hpp"

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace carafe::server {

namespace {

// No default label, so a new enumerator breaks this build rather than becoming a
// silent 400.
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

// The body names the status: a bare 404 tells a terminal reader nothing.
http::Response status_response(int status) {
    std::string body = std::to_string(status);
    body += ' ';
    body += http::status_message(status);
    body += '\n';
    return http::text_response(status, std::move(body));
}

// connection: close only when the stream cannot be resynchronised. A refusal the
// reader can read past leaves the connection as healthy as a 404 does.
http::Response parse_error_response(http::RequestError error, const bool stream_continues) {
    http::Response response = status_response(status_for(error));
    if (!stream_continues) {
        response.headers.add({"connection", "close"});
    }
    return response;
}

// Comma-separated, as RFC 9110 spells the field. Method names are case-sensitive
// tokens, so these stay uppercase though every field name we emit is lowered.
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

// A path nobody registered is a 404; one registered under another method is a
// 405, and RFC 9110 makes Allow on that 405 a MUST rather than a courtesy.
http::Response unmatched_response(const Router& router, std::string_view target,
                                  bool path_matched) {
    if (!path_matched) {
        return status_response(404);
    }
    http::Response response = status_response(405);
    response.headers.add({"allow", allow_value(router.allowed_methods(target))});
    return response;
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

            const http::Response response =
                parse_error_response(result.error, result.stream_continues);
            if (!conn.write(response.serialize())) {
                return;  // nobody left to answer
            }

            // Only the reader knows whether the next request line is findable, and
            // the reply just sent told the client which answer it gave.
            if (!result.stream_continues) {
                return;
            }
            continue;
        }

        if (!result.request) {
            return;  // the client finished
        }

        http::Request& request = *result.request;
        auto match = router.find(request.method, request.target);

        // Unconditional: an unmatched request captured nothing, so this costs an
        // empty vector rather than a branch.
        request.params = std::move(match.params);

        const http::Response response =
            match ? (*match.handler)(request)
                  : unmatched_response(router, request.target, match.path_matched);

        if (!conn.write(response.serialize(request.method != http::Method::Head))) {
            return;  // nobody left to answer
        }
    }
}

}  // namespace carafe::server
