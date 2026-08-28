#pragma once

#include <carafe/http/headers.hpp>

#include <string>
#include <string_view>

namespace carafe::http {

struct Response {
    int status = 200;
    Headers headers;

    std::string body;

    // Content-Length is computed here and any the caller added is dropped: the one number a client cannot recover from
    // when it is wrong. `with_body` false stops after the head for HEAD, so the length still describes the GET body.
    [[nodiscard]] std::string serialize(bool with_body = true) const;
};

// Advisory: clients branch on the number, so an unknown code gets an empty phrase rather than a guess. RFC 7230 allows
// zero characters there.
[[nodiscard]] std::string_view status_message(int status) noexcept;

// text/plain with the body attached.
[[nodiscard]] Response text_response(int status, std::string body);

}  // namespace carafe::http
