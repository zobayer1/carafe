#pragma once

#include <cstddef>
#include <string_view>

namespace carafe::http {

// HTTP case-insensitivity is ASCII-only, so std::tolower is wrong here: it is locale-dependent, and in a Turkish locale
// 'I' does not lower to 'i'.
[[nodiscard]] constexpr char ascii_lower(char ch) noexcept {
    const auto u_ch = static_cast<unsigned char>(ch);
    if (u_ch >= 'A' && u_ch <= 'Z') {
        return static_cast<char>(u_ch + ('a' - 'A'));
    }
    return ch;
}

// Case-insensitive ASCII compare with the folding done once. `lowered` must already be lowercase: a literal, or a name
// Headers::add folded on the way in.
[[nodiscard]] constexpr bool ascii_equals_lowered(std::string_view lowered, std::string_view text) noexcept {
    if (text.size() != lowered.size()) {
        return false;
    }
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (ascii_lower(text[i]) != lowered[i]) {
            return false;
        }
    }
    return true;
}

// The value of one HEXDIG, or -1 for anything else, so a caller tests the result rather than classifying first.
// std::isxdigit is wrong here for the reasons above, and lowercase counts: RFC 5234 §2.3 makes ABNF terminals
// case-insensitive, so "A"-"F" matches a-f too.
[[nodiscard]] constexpr int ascii_hex_value(char ch) noexcept {
    const auto u_ch = static_cast<unsigned char>(ch);
    if (u_ch >= '0' && u_ch <= '9') {
        return u_ch - '0';
    }
    if (u_ch >= 'a' && u_ch <= 'f') {
        return u_ch - 'a' + 10;
    }
    if (u_ch >= 'A' && u_ch <= 'F') {
        return u_ch - 'A' + 10;
    }
    return -1;
}

}  // namespace carafe::http
