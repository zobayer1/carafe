#pragma once

#include <cstddef>
#include <optional>
#include <string_view>

namespace carafe::http {

// RFC 9110 §5.6.1: a comma-separated list whose elements carry OWS that is not part of them, and where an empty element
// is legal and ignored. Advances `pos` past what it returns, so a caller loops until it gets nothing.
[[nodiscard]] constexpr std::optional<std::string_view> next_list_element(std::string_view value,
                                                                          std::size_t& pos) noexcept {
    while (pos < value.size()) {
        std::size_t end = value.find(',', pos);
        if (end == std::string_view::npos) {
            end = value.size();
        }
        std::string_view element = value.substr(pos, end - pos);
        pos = end + 1;

        while (!element.empty() && (element.front() == ' ' || element.front() == '\t')) {
            element.remove_prefix(1);
        }
        while (!element.empty() && (element.back() == ' ' || element.back() == '\t')) {
            element.remove_suffix(1);
        }

        if (!element.empty()) {
            return element;
        }
    }
    return std::nullopt;
}

}  // namespace carafe::http
