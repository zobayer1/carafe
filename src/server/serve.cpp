#include "server/serve.hpp"

#include <carafe/http/request.hpp>
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
std::string_view status_for(http::RequestError error) {
    switch (error) {
        case http::RequestError::UnknownMethod:
            return "501 Not Implemented";
        case http::RequestError::UnsupportedVersion:
            return "505 HTTP Version Not Supported";
        case http::RequestError::RequestLineTooLong:
            return "414 URI Too Long";
        case http::RequestError::HeaderTooLong:
        case http::RequestError::TooManyHeaders:
        case http::RequestError::HeadTooLarge:
            return "431 Request Header Fields Too Large";
        case http::RequestError::Malformed:
        case http::RequestError::None:
            break;
    }
    return "400 Bad Request";
}

// Content-Length on every response, so the client knows where the body ends
// without waiting for a close -- which is what lets the connection stay open.
std::string response_head(std::string_view status, std::size_t body_size,
                          std::string_view extra_headers = "") {
    std::string out = "HTTP/1.1 ";
    out += status;
    out += "\r\nContent-Type: text/plain; charset=utf-8\r\nContent-Length: ";
    out += std::to_string(body_size);
    out += "\r\n";
    out += extra_headers;
    out += "\r\n";
    return out;
}

std::string text_response(std::string_view status, std::string_view body,
                          std::string_view extra_headers = "") {
    std::string out = response_head(status, body.size(), extra_headers);
    out += body;
    return out;
}

// Connection: close, because the byte stream cannot be resynchronised after a bad
// head. Without it the client waits for a response that will never come.
std::string error_response(http::RequestError error) {
    const std::string_view status = status_for(error);
    std::string body{status};
    body += '\n';
    return text_response(status, body, "Connection: close\r\n");
}

// The body shows the head was really parsed: the version comes from the library,
// the target off the wire.
std::string response_for(const http::Request& request) {
    std::string body = "carafe ";
    body += version();
    body += "\nyou asked for ";
    body += request.target;
    body += '\n';

    // A HEAD answer carries exactly the headers its GET would, and no body.
    if (request.method == http::Method::Head) {
        return response_head("200 OK", body.size());
    }
    return text_response("200 OK", body);
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
                static_cast<void>(conn.write(error_response(result.error)));
            }
            return;
        }

        if (!result.request) {
            return;  // the client finished
        }

        if (!conn.write(response_for(*result.request))) {
            return;  // nobody left to answer
        }
    }
}

}  // namespace carafe::server
