#pragma once

namespace carafe::http {

// HTTP case-insensitivity is ASCII-only, so std::tolower is wrong here: it is
// locale-dependent, and in a Turkish locale 'I' does not lower to 'i'.
[[nodiscard]] constexpr char ascii_lower(char ch) noexcept {
    const auto u_ch = static_cast<unsigned char>(ch);
    if (u_ch >= 'A' && u_ch <= 'Z') {
        return static_cast<char>(u_ch + ('a' - 'A'));
    }
    return ch;
}

}  // namespace carafe::http
