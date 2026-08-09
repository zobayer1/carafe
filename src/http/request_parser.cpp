#include "http/request_parser.hpp"

#include <array>
#include <cctype>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace carafe::http {

namespace {

// Maps a method token to Method; nullopt means we do not implement that verb.
std::optional<Method> lookup_method(std::string_view token) noexcept {
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
ParseError parse_version(std::string_view token, Version& out) noexcept {
    if (token.size() != 8 || token.substr(0, 5) != "HTTP/" || token[6] != '.' ||
        !std::isdigit(static_cast<unsigned char>(token[5])) ||
        !std::isdigit(static_cast<unsigned char>(token[7]))) {
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

    std::string_view method_part = line.substr(0, sp1);
    std::string_view target_part = line.substr(sp1 + 1, sp2 - sp1 - 1);
    std::string_view version_part = line.substr(sp2 + 1);

    if (method_part.empty() || target_part.empty() || version_part.empty()) {
        return {ParseError::Malformed, {}};
    }

    Version version{};
    // Version test first: ensure that we understand the protocol version.
    if (const auto err = parse_version(version_part, version); err != ParseError::None) {
        return {err, {}};
    }

    const auto method_opt = lookup_method(method_part);
    if (!method_opt.has_value()) {
        return {ParseError::UnknownMethod, {}};
    }

    return {ParseError::None, RequestLine{*method_opt, std::string{target_part}, version}};
}

}  // namespace carafe::http
