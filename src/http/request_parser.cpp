#include "http/request_parser.hpp"

#include <carafe/http/headers.hpp>
#include <carafe/http/request.hpp>

#include "http/ascii.hpp"

#include <array>
#include <cctype>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace carafe::http {

namespace {

// Maps a method token to Method; nullopt means we do not implement that verb.
[[nodiscard]] std::optional<Method> lookup_method(std::string_view token) noexcept {
    static constexpr std::array<std::pair<std::string_view, Method>, 9> table{
        {{"GET", Method::Get},
         {"HEAD", Method::Head},
         {"POST", Method::Post},
         {"PUT", Method::Put},
         {"DELETE", Method::Delete},
         {"CONNECT", Method::Connect},
         {"OPTIONS", Method::Options},
         {"TRACE", Method::Trace},
         {"PATCH", Method::Patch}}};

    for (const auto& [name, method] : table) {
        if (token == name) {
            return method;
        }
    }
    return std::nullopt;
}

// Shape before value: a table alone cannot tell "not a version" (400) from "a
// version we do not speak" (505).
[[nodiscard]] ParseError parse_version(std::string_view token, Version& out) noexcept {
    if (token.size() != 8 || token.substr(0, 5) != "HTTP/" || token[6] != '.' ||
        std::isdigit(static_cast<unsigned char>(token[5])) == 0 ||
        std::isdigit(static_cast<unsigned char>(token[7])) == 0) {
        return ParseError::Malformed;
    }

    static constexpr std::array<std::pair<std::string_view, Version>, 2> table{
        {{"HTTP/1.0", Version::Http10}, {"HTTP/1.1", Version::Http11}}};

    for (const auto& [name, version] : table) {
        if (token == name) {
            out = version;
            return ParseError::None;
        }
    }

    return ParseError::UnsupportedVersion;
}

// RFC 9112 2.2: a surviving bare CR lets a proxy and this server disagree about
// where the line ends. Rejecting all CTLs also covers LF, NUL, and tab; SP is
// not a CTL, so this is independent of the field split.
[[nodiscard]] bool contains_ctl(std::string_view text) noexcept {
    for (const char ch : text) {
        const auto u_ch = static_cast<unsigned char>(ch);
        if (u_ch < 0x20 || u_ch == 0x7F) {
            return true;
        }
    }
    return false;
}

// RFC 3986 §2.1: pct-encoded = "%" HEXDIG HEXDIG. Both hex cases are accepted
// because §6.2.2.1 makes them equivalent.
[[nodiscard]] constexpr bool is_hex_digit(unsigned char ch) noexcept {
    return (ch >= '0' && ch <= '9') || (ch >= 'A' && ch <= 'F') || (ch >= 'a' && ch <= 'f');
}

// RFC 9110 §5.6.2: tchar = ALPHA / DIGIT / "!" / "#" / "$" / "%" / "&" /
// "'" / "*" / "+" / "-" / "." / "^" / "_" / "`" / "|" / "~"
[[nodiscard]] constexpr bool is_tchar(unsigned char ch) noexcept {
    if ((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9')) {
        return true;
    }
    switch (ch) {
        case '!':
        case '#':
        case '$':
        case '%':
        case '&':
        case '\'':
        case '*':
        case '+':
        case '-':
        case '.':
        case '^':
        case '_':
        case '`':
        case '|':
        case '~':
            return true;
        default:
            return false;
    }
}

// RFC 9110 §5.6.3: OWS = *( SP / HTAB )
[[nodiscard]] constexpr bool is_ows(unsigned char ch) noexcept {
    return ch == ' ' || ch == '\t';
}

// RFC 9110 §5.5: field-vchar = VCHAR / obs-text, with SP and HTAB between them.
// An allowlist, so a control byte nobody thought to name still fails.
[[nodiscard]] constexpr bool is_valid_value_char(unsigned char ch) noexcept {
    return ch == '\t' || (ch >= 0x20 && ch != 0x7F);
}

// A bare '%' is not a byte a URI may carry, so there is no lenient reading to
// fall back to. Rejecting the whole target here is what lets the router decode
// a capture with no branch for input it cannot represent.
[[nodiscard]] bool has_valid_escapes(std::string_view target) noexcept {
    for (std::size_t i = 0; i < target.size(); ++i) {
        if (target[i] != '%') {
            continue;
        }
        // The size test guards both indexings: string_view does not bounds check,
        // so a '%' in the last two bytes would otherwise read past the end.
        if (i + 2 >= target.size() || !is_hex_digit(static_cast<unsigned char>(target[i + 1])) ||
            !is_hex_digit(static_cast<unsigned char>(target[i + 2]))) {
            return false;
        }
        i += 2;
    }
    return true;
}

}  // namespace

// Validates entirely on views; the one allocation happens only once the line is
// known to be good.
RequestLineResult parse_request_line(std::string_view line) {
    const auto sp1 = line.find(' ');
    if (sp1 == std::string_view::npos) {
        return {ParseError::Malformed, {}};
    }

    const auto sp2 = line.find(' ', sp1 + 1);
    if (sp2 == std::string_view::npos) {
        return {ParseError::Malformed, {}};
    }

    // Check for control characters in the entire line.
    if (contains_ctl(line)) {
        return {ParseError::Malformed, {}};
    }

    // Check for extra spaces or malformed trailing data.
    if (line.find(' ', sp2 + 1) != std::string_view::npos) {
        return {ParseError::Malformed, {}};
    }

    const std::string_view method_part = line.substr(0, sp1);
    const std::string_view target_part = line.substr(sp1 + 1, sp2 - sp1 - 1);
    const std::string_view version_part = line.substr(sp2 + 1);

    if (method_part.empty() || target_part.empty() || version_part.empty()) {
        return {ParseError::Malformed, {}};
    }

    Version version{};
    // Version test first: ensure that we understand the protocol version.
    if (const auto err = parse_version(version_part, version); err != ParseError::None) {
        return {err, {}};
    }

    // Ahead of the method lookup: 501 promises a request we could read carrying a
    // verb we do not implement, and a target we cannot read breaks that promise.
    if (!has_valid_escapes(target_part)) {
        return {ParseError::Malformed, {}};
    }

    const auto method_opt = lookup_method(method_part);
    if (!method_opt.has_value()) {
        return {ParseError::UnknownMethod, {}};
    }

    return {ParseError::None, RequestLine{*method_opt, std::string{target_part}, version}};
}

// Parses "field-name ':' OWS field-value OWS". `line` must arrive with its
// CRLF removed and length-capped by the caller; capping here would be too late.
std::optional<HeaderField> parse_header_field(std::string_view line) {
    // A colon is ordinary content in a value, so only the first one splits.
    const auto colon_pos = line.find(':');
    if (colon_pos == std::string_view::npos) {
        return std::nullopt;
    }

    const std::string_view name_part = line.substr(0, colon_pos);
    if (name_part.empty()) {
        return std::nullopt;
    }

    std::string lower_name;
    lower_name.reserve(name_part.size());

    for (const char ch : name_part) {
        const auto u_ch = static_cast<unsigned char>(ch);
        if (!is_tchar(u_ch)) {
            return std::nullopt;
        }
        lower_name.push_back(ascii_lower(ch));
    }

    const std::string_view value_part = line.substr(colon_pos + 1);

    // OWS is optional on both sides, and `end > start` keeps an all-OWS value
    // from walking the second scan off the front.
    std::size_t start = 0;
    while (start < value_part.size() && is_ows(static_cast<unsigned char>(value_part[start]))) {
        ++start;
    }
    std::size_t end = value_part.size();
    while (end > start && is_ows(static_cast<unsigned char>(value_part[end - 1]))) {
        --end;
    }

    // Over the untrimmed span: the bytes being discarded still have to be legal.
    for (const char ch : value_part) {
        if (!is_valid_value_char(static_cast<unsigned char>(ch))) {
            return std::nullopt;
        }
    }

    return HeaderField{std::move(lower_name), std::string{value_part.substr(start, end - start)}};
}

}  // namespace carafe::http
