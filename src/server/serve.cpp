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

// The mapping RequestError was written for: every value has carried its status
// in a comment since the enum existed, and nothing until now could act on it.
// No default label, so adding an enumerator becomes a warning here rather than
// a silent 400.
int status_for(http::RequestError error) {
    switch (error) {
        case http::RequestError::UnknownMethod:
            return 501;
        case http::RequestError::UnsupportedVersion:
            return 505;
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

// The body names the status, because a bare 404 tells a person reading a
// terminal nothing at all.
http::Response status_response(int status) {
    std::string body = std::to_string(status);
    body += ' ';
    body += http::status_message(status);
    body += '\n';
    return http::text_response(status, std::move(body));
}

// connection: close, because the byte stream cannot be resynchronised after a
// bad head. A 404 gets no such header -- that connection is perfectly healthy.
http::Response parse_error_response(http::RequestError error) {
    http::Response response = status_response(status_for(error));
    response.headers.add({"connection", "close"});
    return response;
}

// Comma-separated, as RFC 9110 spells the field. Method names are case-sensitive
// tokens: these stay uppercase even though every field name we emit is lowered.
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
            // Answerable or not, the connection ends here: the byte stream cannot
            // be resynchronised after a bad head, and RequestReader latches the
            // failure. A read failure gets no reply because nobody is listening.
            if (result.os_error == 0) {
                static_cast<void>(conn.write(parse_error_response(result.error).serialize()));
            }
            return;
        }

        if (!result.request) {
            return;  // the client finished
        }

        http::Request& request = *result.request;
        auto match = router.find(request.method, request.target);

        // Whatever the router bound, unconditionally: an unmatched request captured
        // nothing, so this costs an empty vector rather than a branch.
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
