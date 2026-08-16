#include <carafe/http/response.hpp>

#include <string>
#include <string_view>

namespace carafe::http {

std::string_view status_message(int status) noexcept {
    switch (status) {
        case 200:
            return "OK";
        case 400:
            return "Bad Request";
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

std::string Response::serialize(bool with_body) const {
    std::string out = "HTTP/1.1 ";
    out += std::to_string(status) + " ";
    out += status_message(status);
    out += "\r\n";

    for (const HeaderField& field : headers) {
        // Ours is written below from body.size(). A caller's is either the same
        // number or a lie the client has no way to recover from.
        if (field.name == "content-length") {
            continue;
        }
        out += field.name + ": " + field.value + "\r\n";
    }

    // Lowercased like every other name here, since Headers::add lowercases the
    // ones it is given and a mixed wire implies a distinction HTTP does not make.
    out += "content-length: " + std::to_string(body.size()) + "\r\n\r\n";

    if (with_body) {
        out += body;
    }

    return out;
}

}  // namespace carafe::http
