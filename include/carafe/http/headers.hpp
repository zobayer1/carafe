#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace carafe::http {

struct HeaderField {
    std::string name;   // lowercased
    std::string value;  // OWS-trimmed
};

// Field order is preserved: repeated fields are legal, and reserialising a
// request depends on the order they arrived in.
class Headers {
public:
    using const_iterator = std::vector<HeaderField>::const_iterator;

    // Lowercases the name, so callers cannot break lookup. Invalidates every
    // view previously returned by get(): the vector reallocates.
    void add(HeaderField field);

    // The first value stored under `name`, which may repeat. Lookup ignores
    // case. The view lives as long as this Headers goes unmodified.
    [[nodiscard]] std::optional<std::string_view> get(std::string_view name) const noexcept;

    [[nodiscard]] bool contains(std::string_view name) const noexcept;

    // What makes the singleton rules checkable: exactly one Host, no
    // conflicting Content-Length.
    [[nodiscard]] std::size_t count(std::string_view name) const noexcept;

    [[nodiscard]] std::size_t size() const noexcept {
        return fields_.size();
    }

    [[nodiscard]] bool empty() const noexcept {
        return fields_.empty();
    }

    [[nodiscard]] const_iterator begin() const noexcept {
        return fields_.begin();
    }

    [[nodiscard]] const_iterator end() const noexcept {
        return fields_.end();
    }

private:
    std::vector<HeaderField> fields_;
};

}  // namespace carafe::http
