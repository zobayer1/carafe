#include "http/path.hpp"

#include "http/ascii.hpp"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace carafe::http {

namespace {

// Only the unreserved ones, and that is the point: "%2F" survives as an escape, so a client cannot spell a segment
// boundary that the split below will not see.
[[nodiscard]] std::string resolve_escapes(std::string_view path) {
    std::string out;
    out.reserve(path.size());  // resolving only ever shrinks

    std::size_t pos = 0;
    while (pos < path.size()) {
        if (path[pos] != '%') {
            out += path[pos];
            ++pos;
            continue;
        }
        // Signed, and two steps rather than one condition: ascii_hex_value reports -1, and the bounds test has to
        // reach the line below rather than be lost to a type that cannot hold it.
        const int high = (pos + 2 < path.size()) ? ascii_hex_value(path[pos + 1]) : -1;
        const int low = (high < 0) ? -1 : ascii_hex_value(path[pos + 2]);
        if (low < 0) {
            out += path[pos];
            ++pos;
            continue;
        }

        const int byte = (high << 4) | low;
        if (ascii_unreserved(static_cast<char>(byte))) {
            out += static_cast<char>(byte);
        } else {
            out += '%';
            out += ascii_hex_digit(high);
            out += ascii_hex_digit(low);
        }

        pos += 3;
    }
    return out;
}

// The leading slash is taken off before the walk and put back after, which is the whole of the root clamp: with
// nothing kept, ".." has nothing to pop, so no target can climb out.
[[nodiscard]] std::string remove_dot_segments(std::string_view path) {
    const bool absolute = !path.empty() && path[0] == '/';
    const std::string_view body = absolute ? path.substr(1) : path;

    // Views into `body`, so this walk must finish before the string behind it goes.
    std::vector<std::string_view> kept;
    bool ended_on_dots = false;

    // Through the end, not up to it: a body ending in '/' owes one more empty segment, which is the trailing slash.
    std::size_t pos = 0;
    while (pos <= body.size()) {
        std::size_t next_slash = body.find('/', pos);
        if (next_slash == std::string_view::npos) {
            next_slash = body.size();
        }

        const std::string_view segment = body.substr(pos, next_slash - pos);

        if (segment == ".") {
            ended_on_dots = true;
        } else if (segment == "..") {
            if (!kept.empty()) {
                kept.pop_back();
            }
            ended_on_dots = true;
        } else {
            kept.push_back(segment);
            ended_on_dots = false;
        }

        pos = next_slash + 1;
    }

    // RFC 3986 §5.2.4 leaves a trailing slash where the last segment was a dot segment: "/a/b/.." is "/a/", not "/a".
    // An empty segment says that without a case of its own, and it is what makes "/.." come back as "/".
    if (ended_on_dots) {
        kept.emplace_back("");
    }

    std::string joined = absolute ? "/" : "";
    for (std::size_t i = 0; i < kept.size(); ++i) {
        if (i > 0) {
            joined += "/";
        }
        joined += kept[i];
    }
    return joined;
}

}  // namespace

std::string normalize_path(std::string_view path) {
    // Nested rather than named: the inner string lives to the end of this statement, which is exactly as long as the
    // views taken into it are used. Returning a view here instead of a string would hand back a dangling one.
    return remove_dot_segments(resolve_escapes(path));
}

}  // namespace carafe::http
