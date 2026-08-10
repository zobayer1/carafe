#include <carafe/http/headers.hpp>

#include "http/ascii.hpp"

#include <cstddef>
#include <optional>
#include <string_view>
#include <utility>

namespace carafe::http {

namespace {

// Compares a stored lowercased name to a lookup name, ignoring case.
// The stored name is lowercased, so the lookup name is lowercased on-the-fly.
[[nodiscard]] bool matches_lowered(std::string_view stored_lower,
                                   std::string_view lookup_name) noexcept {
    if (stored_lower.size() != lookup_name.size()) {
        return false;
    }
    for (std::size_t i = 0; i < stored_lower.size(); ++i) {
        auto u_lookup = static_cast<unsigned char>(lookup_name[i]);
        if (u_lookup >= 'A' && u_lookup <= 'Z') {
            u_lookup += ('a' - 'A');
        }
        if (stored_lower[i] != static_cast<char>(u_lookup)) {
            return false;
        }
    }
    return true;
}

}  // namespace

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
        if (matches_lowered(field.name, name)) {
            ++matches;
        }
    }
    return matches;
}

std::optional<std::string_view> Headers::get(std::string_view name) const noexcept {
    for (const auto& field : fields_) {
        if (matches_lowered(field.name, name)) {
            return field.value;
        }
    }
    return std::nullopt;
}

bool Headers::contains(std::string_view name) const noexcept {
    return get(name).has_value();
}

}  // namespace carafe::http
