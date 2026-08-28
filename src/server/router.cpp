#include "server/router.hpp"

#include <carafe/http/handler.hpp>
#include <carafe/http/request.hpp>

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
std::string_view path_of(std::string_view target) noexcept {
    return target.substr(0, target.find('?'));
}

[[nodiscard]] constexpr int hex_value(char ch) noexcept {
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

// An escape that is not "%" HEXDIG HEXDIG is copied as it stands. Unreachable in a served request, since the parser
// rejects those first. Even so, find() states no precondition, and a decoder assuming one reads past the end for "%4".
[[nodiscard]] std::string percent_decode(std::string_view text) {
    std::string decoded;
    decoded.reserve(text.size());  // decoding only ever shrinks

    for (size_type i = 0; i < text.size(); ++i) {
        // Two steps, not one condition: text[i + 2] has to stay behind the bounds test, and only the first line carries
        // it.
        const int high = i + 2 < text.size() ? hex_value(text[i + 1]) : -1;
        const int low = high < 0 ? -1 : hex_value(text[i + 2]);

        if (text[i] != '%' || low < 0) {
            decoded.push_back(text[i]);
            continue;
        }
        decoded.push_back(static_cast<char>((high << 4) | low));
        i += 2;
    }
    return decoded;
}

// One definition of "this route serves this path", so find() and allowed_methods() cannot disagree. Captures into *out
// on success only, so a half-match leaves no debris; null asks for the yes or no alone.
bool matches(const Pattern& pattern, std::string_view path, http::Params* out) {
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

        if (segment.is_param) {
            // A parameter has to stand for something: "/users//" binds no id.
            if (piece.empty()) {
                return false;
            }
            if (out != nullptr) {
                captured.entries.push_back({segment.text, percent_decode(piece)});
            }
        } else if (segment.text != piece) {
            return false;
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

bool contains(const std::vector<http::Method>& methods, http::Method method) {
    return std::find(methods.begin(), methods.end(), method) != methods.end();
}

// The leading empty segment is kept rather than skipped, so a pattern and a request path cut the same way and the walk
// needs no case for the root.
Pattern compile(std::string_view path) {
    Pattern pattern;
    size_type start = 0;
    while (true) {
        const size_type end = path.find('/', start);
        const std::string_view segment_text = path.substr(start, end - start);
        const bool is_param = segment_text.length() >= 3 && segment_text.front() == '<' && segment_text.back() == '>';
        const std::string_view text = is_param ? segment_text.substr(1, segment_text.size() - 2) : segment_text;
        pattern.push_back({std::string(text), is_param});

        if (end == std::string_view::npos) {
            break;
        }
        start = end + 1;
    }
    return pattern;
}

}  // namespace

void Router::add(http::Method method, std::string_view path, http::Handler handler) {
    routes_.push_back({method, compile(path), std::move(handler)});
}

Match Router::find(http::Method method, std::string_view target) const {
    const std::string_view path = path_of(target);
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
    const std::string_view path = path_of(target);
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
