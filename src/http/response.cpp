#include <carafe/http/response.hpp>

#include <string>
#include <string_view>
#include <utility>

namespace carafe::http {

std::string_view status_message(int status) noexcept {
    switch (status) {
        case 200:
            return "OK";
        case 400:
            return "Bad Request";
        case 404:
            return "Not Found";
        case 405:
            return "Method Not Allowed";
        case 408:
            return "Request Timeout";
        case 413:
            return "Content Too Large";
        case 414:
            return "URI Too Long";
        case 431:
            return "Request Header Fields Too Large";
        case 501:
            return "Not Implemented";
        case 505:
            return "HTTP Version Not Supported";
        default:
            return "";
    }
}

Response text_response(int status, std::string body) {
    Response response;
    response.status = status;
    response.headers.add({"content-type", "text/plain; charset=utf-8"});
    response.body = std::move(body);
    return response;
}

std::string Response::serialize(bool with_body) const {
    std::string out = "HTTP/1.1 ";
    out += std::to_string(status) + " ";
    out += status_message(status);
    out += "\r\n";

    for (const HeaderField& field : headers) {
        // Written below from body.size(). A caller's is the same number or a lie the client cannot recover from.
        if (field.name == "content-length") {
            continue;
        }
        out += field.name + ": " + field.value + "\r\n";
    }

    // Lowercased like every other name here: Headers::add folds the ones it is given, and a mixed wire implies a
    // distinction HTTP does not make.
    out += "content-length: " + std::to_string(body.size()) + "\r\n\r\n";

    if (with_body) {
        out += body;
    }

    return out;
}

}  // namespace carafe::http
