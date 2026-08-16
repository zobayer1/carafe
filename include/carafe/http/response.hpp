#pragma once

#include <carafe/http/headers.hpp>

#include <string>
#include <string_view>

namespace carafe::http {

struct Response {
    int status = 200;
    Headers headers;

    std::string body;

    // Content-Length is computed here and any the caller added is dropped: it is
    // the one number a client cannot recover from when it is wrong. `with_body`
    // false stops after the head, which is what HEAD asks for -- and why the
    // length still describes the body a GET would have carried.
    [[nodiscard]] std::string serialize(bool with_body = true) const;
};

// Advisory: clients branch on the number, so an unknown code gets an empty
// phrase rather than a guess. RFC 7230 allows zero characters there.
[[nodiscard]] std::string_view status_message(int status) noexcept;

// text/plain with the body attached, which is what a handler writing prose
// wants and what it would otherwise assemble by hand every time.
[[nodiscard]] Response text_response(int status, std::string body);

}  // namespace carafe::http
