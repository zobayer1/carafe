#include "server/router.hpp"

#include <carafe/http/handler.hpp>
#include <carafe/http/request.hpp>

#include "http/ascii.hpp"
#include "http/path.hpp"

#include <algorithm>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace carafe::server {

namespace {

using size_type = std::string_view::size_type;

// The request-target carries the query; routing does not use it. Clients strip fragments before sending, so '?' is the
// only cut.
[[nodiscard]] std::string_view path_of(std::string_view target) noexcept {
    return target.substr(0, target.find('?'));
}

// True of an empty text, which is what "all" says over nothing. The walk refuses an empty segment before asking, so a
// parameter still stands for something.
[[nodiscard]] bool all_digits(std::string_view text) noexcept {
    for (const char ch : text) {
        if (ch < '0' || ch > '9') {
            return false;
        }
    }
    return true;
}

// An escape that is not "%" HEXDIG HEXDIG is copied as it stands. Unreachable in a served request, since the parser
// rejects those first. Even so, find() states no precondition, and a decoder assuming one reads past the end for "%4".
[[nodiscard]] std::string percent_decode(std::string_view text) {
    std::string decoded;
    decoded.reserve(text.size());  // decoding only ever shrinks

    for (size_type i = 0; i < text.size(); ++i) {
        // Two steps, not one condition: text[i + 2] has to stay behind the bounds test, and only the first line carries
        // it.
        const int high = i + 2 < text.size() ? http::ascii_hex_value(text[i + 1]) : -1;
        const int low = high < 0 ? -1 : http::ascii_hex_value(text[i + 2]);

        if (text[i] != '%' || low < 0) {
            decoded.push_back(text[i]);
            continue;
        }
        decoded.push_back(static_cast<char>((high << 4) | low));
        i += 2;
    }
    return decoded;
}

// No default: a new converter has to say whether it binds a name, rather than inherit an answer from the last one.
[[nodiscard]] constexpr bool binds_a_name(Capture capture) noexcept {
    switch (capture) {
        case Capture::None:
            return false;
        case Capture::Text:
        case Capture::Number:
        case Capture::Rest:
            return true;
    }
    return false;
}

// Whether one piece of the path is what this segment accepts. Rest is never asked: it takes more than one piece, so
// matches() deals with it first. No default, so a new converter has to say what it accepts.
[[nodiscard]] bool accepts(const Segment& segment, std::string_view piece) noexcept {
    switch (segment.capture) {
        case Capture::None:
            return segment.text == piece;
        case Capture::Text:
            return true;
        case Capture::Number:
            return all_digits(piece);
        case Capture::Rest:
            return false;
    }
    return false;
}

// One definition of "this route serves this path", so find() and allowed_methods() cannot disagree. Captures into *out
// on success only, so a half-match leaves no debris; null asks for the yes or no alone.
[[nodiscard]] bool matches(const Pattern& pattern, std::string_view path, http::Params* out) {
    http::Params captured;
    size_type index = 0;
    size_type start = 0;
    while (true) {
        const size_type end = path.find('/', start);
        const std::string_view piece = path.substr(start, end - start);

        if (index == pattern.size()) {
            return false;
        }

        const Segment& segment = pattern[index];

        // A parameter has to stand for something, whatever it was going to accept: "/users//" binds no id. For a rest
        // it is also what keeps the capture from beginning with '/', which a path join would read as absolute.
        if (binds_a_name(segment.capture) && piece.empty()) {
            return false;
        }

        // Consumes what is left of the path, so the walk ends here. A pattern with more segments after it never
        // matches, and the length check below the loop already reports that.
        if (segment.capture == Capture::Rest) {
            const std::string_view rest = path.substr(start);
            std::string decoded = percent_decode(rest);

            // Decoding only ever adds separators. One that appears here is a boundary the walk never saw, which is how
            // "a%2F..%2Fb" would put back the dot segment normalisation removed.
            if (std::count(decoded.begin(), decoded.end(), '/') != std::count(rest.begin(), rest.end(), '/')) {
                return false;
            }

            if (out != nullptr) {
                captured.entries.push_back({segment.text, std::move(decoded)});
            }
            ++index;
            break;
        }

        if (!accepts(segment, piece)) {
            return false;
        }

        if (binds_a_name(segment.capture) && out != nullptr) {
            captured.entries.push_back({segment.text, percent_decode(piece)});
        }

        ++index;

        if (end == std::string_view::npos) {
            break;
        }
        start = end + 1;
    }
    if (index != pattern.size()) {
        return false;
    }
    if (out != nullptr) {
        *out = std::move(captured);
    }
    return true;
}

[[nodiscard]] bool contains(const std::vector<http::Method>& methods, http::Method method) noexcept {
    return std::find(methods.begin(), methods.end(), method) != methods.end();
}

// The converters a pattern may name. Unknown is Capture::None, which compile_segment reads as "not a parameter".
[[nodiscard]] Capture capture_for(std::string_view converter) noexcept {
    if (converter == "str") {
        return Capture::Text;
    }
    if (converter == "int") {
        return Capture::Number;
    }
    if (converter == "path") {
        return Capture::Rest;
    }
    return Capture::None;
}

// A parameter is "<name>", or "<converter:name>" naming a converter this file knows and binding a name that is not
// empty. Anything else between brackets is literal text, which is already what "<>" and a half-bracketed segment
// become: add() has no channel to refuse a pattern on.
[[nodiscard]] Segment compile_segment(std::string_view text) {
    if (text.size() < 3 || text.front() != '<' || text.back() != '>') {
        return {std::string(text), Capture::None};
    }
    const std::string_view inner = text.substr(1, text.size() - 2);
    const size_type colon = inner.find(':');
    if (colon == std::string_view::npos) {
        return {std::string(inner), Capture::Text};
    }
    const Capture capture = capture_for(inner.substr(0, colon));
    const std::string_view name = inner.substr(colon + 1);
    if (capture == Capture::None || name.empty()) {
        return {std::string(text), Capture::None};
    }
    return {std::string(name), capture};
}

// The leading empty segment is kept rather than skipped, so a pattern and a request path cut the same way and the walk
// needs no case for the root.
[[nodiscard]] Pattern compile(std::string_view path) {
    Pattern pattern;
    size_type start = 0;
    while (true) {
        const size_type end = path.find('/', start);
        const std::string_view segment_text = path.substr(start, end - start);
        pattern.push_back(compile_segment(segment_text));

        if (end == std::string_view::npos) {
            break;
        }
        start = end + 1;
    }
    return pattern;
}

// The path as a lookup compares it: the query cut off, then normalised, so a target and a pattern meet in one
// spelling. Owned rather than viewed, since normalising may rewrite the bytes.
[[nodiscard]] std::string routable_path(std::string_view target) {
    return http::normalize_path(path_of(target));
}

}  // namespace

void Router::add(http::Method method, std::string_view path, http::Handler handler) {
    routes_.push_back({method, compile(http::normalize_path(path)), std::move(handler)});
}

Match Router::find(http::Method method, std::string_view target) const {
    const std::string path = routable_path(target);
    const http::Handler* fallback = nullptr;
    http::Params fallback_params;
    bool path_matched = false;

    for (const Route& route : routes_) {
        http::Params captured;
        if (!matches(route.pattern, path, &captured)) {
            continue;
        }

        path_matched = true;

        // Returned at once, so the first registration of a method wins.
        if (route.method == method) {
            return {&route.handler, true, std::move(captured)};
        }

        // RFC 9110: HEAD is GET without the body. Remembered rather than returned, so an explicit HEAD route still
        // beats it. Its captures go too, since they exist only during this pass of the loop.
        if (method == http::Method::Head && route.method == http::Method::Get && fallback == nullptr) {
            fallback = &route.handler;
            fallback_params = std::move(captured);
        }
    }
    if (fallback != nullptr) {
        return {fallback, true, std::move(fallback_params)};
    }
    return {nullptr, path_matched, {}};
}

std::vector<http::Method> Router::allowed_methods(std::string_view target) const {
    const std::string path = routable_path(target);
    std::vector<http::Method> allowed;

    for (const Route& route : routes_) {
        if (!matches(route.pattern, path, nullptr)) {
            continue;
        }

        if (!contains(allowed, route.method)) {
            allowed.push_back(route.method);
        }

        // Beside the Get that implies it, not appended, so the order still reads as registration order.
        if (route.method == http::Method::Get && !contains(allowed, http::Method::Head)) {
            allowed.push_back(http::Method::Head);
        }
    }
    return allowed;
}

}  // namespace carafe::server
