#pragma once

#include <carafe/http/headers.hpp>
#include <carafe/http/request.hpp>

#include <optional>
#include <string_view>

namespace carafe::http {

// Each value maps to exactly one HTTP status: 400, 501, 505.
enum class ParseError {
    None,
    Malformed,
    UnknownMethod,
    UnsupportedVersion,
};

struct RequestLineResult {
    ParseError error = ParseError::None;
    RequestLine value;

    [[nodiscard]] explicit operator bool() const noexcept {
        return error == ParseError::None;
    }
};

// Parses "method SP request-target SP HTTP-version". `line` must arrive with its CRLF removed and length-capped by the
// caller; capping here would be too late.
[[nodiscard]] RequestLineResult parse_request_line(std::string_view line);

// Parses "field-name ':' OWS field-value OWS". `line` must arrive with its CRLF removed and length-capped by the
// caller; capping here would be too late.
[[nodiscard]] std::optional<HeaderField> parse_header_field(std::string_view line);

}  // namespace carafe::http
