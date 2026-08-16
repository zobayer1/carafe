#include "server/serve.hpp"

#include <carafe/http/request.hpp>
#include <carafe/http/response.hpp>
#include <carafe/version.hpp>

#include "http/request_reader.hpp"
#include "server/connection.hpp"

#include <cstddef>
#include <string>
#include <string_view>

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

// text/plain is this server's policy, not HTTP's -- serialize sends the headers
// it is handed and adds none of its own but the length.
http::Response text_response(int status, std::string body) {
    http::Response response;
    response.status = status;
    response.headers.add({"content-type", "text/plain; charset=utf-8"});
    response.body = std::move(body);
    return response;
}

// Connection: close, because the byte stream cannot be resynchronised after a bad
// head. Without it the client waits for a response that will never come.
http::Response error_response(http::RequestError error) {
    const int status = status_for(error);

    std::string body = std::to_string(status);
    body += ' ';
    body += http::status_message(status);
    body += '\n';

    http::Response response = text_response(status, std::move(body));
    response.headers.add({"connection", "close"});
    return response;
}

// The body shows the head was really parsed: the version comes from the library,
// the target off the wire.
http::Response response_for(const http::Request& request) {
    std::string body = "carafe ";
    body += version();
    body += "\nyou asked for ";
    body += request.target;
    body += '\n';

    return text_response(200, std::move(body));
}

}  // namespace

void serve_connection(Connection& conn) {
    while (true) {
        const auto result = conn.next_request();

        if (!result) {
            // Answerable or not, the connection ends here: the byte stream cannot
            // be resynchronised after a bad head, and RequestReader latches the
            // failure. A read failure gets no reply because nobody is listening.
            if (result.os_error == 0) {
                static_cast<void>(conn.write(error_response(result.error).serialize()));
            }
            return;
        }

        if (!result.request) {
            return;  // the client finished
        }

        const http::Response response = response_for(*result.request);
        const bool with_body = result.request->method != http::Method::Head;
        if (!conn.write(response.serialize(with_body))) {
            return;  // nobody left to answer
        }
    }
}

}  // namespace carafe::server
