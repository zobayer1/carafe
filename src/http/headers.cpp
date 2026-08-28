#include <carafe/http/headers.hpp>

#include "http/ascii.hpp"

#include <cstddef>
#include <optional>
#include <string_view>
#include <utility>

namespace carafe::http {

void Headers::add(HeaderField field) {
    // The invariant is enforced here so lookup only has to fold one side.
    for (char& ch : field.name) {
        ch = ascii_lower(ch);
    }
    fields_.push_back(std::move(field));
}

std::size_t Headers::count(std::string_view name) const noexcept {
    std::size_t matches = 0;
    for (const auto& field : fields_) {
        if (ascii_equals_lowered(field.name, name)) {
            ++matches;
        }
    }
    return matches;
}

std::optional<std::string_view> Headers::get(std::string_view name) const noexcept {
    for (const auto& field : fields_) {
        if (ascii_equals_lowered(field.name, name)) {
            return field.value;
        }
    }
    return std::nullopt;
}

bool Headers::contains(std::string_view name) const noexcept {
    return get(name).has_value();
}

}  // namespace carafe::http
