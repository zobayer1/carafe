#pragma once

#include <carafe/http/handler.hpp>
#include <carafe/http/request.hpp>

#include <string>
#include <string_view>
#include <vector>

namespace carafe::server {

// What a pattern segment accepts. None is literal text, and is where an unreadable parameter lands.
enum class Capture {
    None,
    Text,    // one segment, any non-empty
    Number,  // one segment, ASCII digits only
};

// One segment of a path pattern: literal text, or the name a capture binds to and what it will accept.
struct Segment {
    std::string text;
    Capture capture = Capture::None;
};

// Split on '/' once, at registration, so matching never re-parses.
using Pattern = std::vector<Segment>;

// Three outcomes, not two: a path registered under another method owes a 405 not a 404, and a null handler alone cannot
// say which. The handler lives as long as the Router is unmodified: add() may reallocate, so register before serving.
struct Match {
    const http::Handler* handler = nullptr;
    bool path_matched = false;

    // What the pattern captured, empty for a static route. Owned rather than viewed, so nothing here depends on the
    // target outliving the lookup.
    http::Params params;

    [[nodiscard]] explicit operator bool() const noexcept {
        return handler != nullptr;
    }
};

class Router {
public:
    // A segment spelled "<name>" captures that segment under that name, and "<str:name>" says so explicitly.
    // "<int:name>" captures only a segment of ASCII digits, so "/users/<int:id>" and "/users/<name>" can both be
    // registered and the digits decide which one answers. A converter constrains matching alone: a capture is still
    // text, and a handler wanting a number parses it. Anything else between angle brackets is literal text, an unknown
    // converter and an empty name included.
    //
    // Appended, and find() scans in order, so a path registered twice keeps its first handler and the first pattern
    // that matches wins however specific a later one is. Harmless, rather than an error with no channel to report on.
    void add(http::Method method, std::string_view path, http::Handler handler);

    // The target is expected to have come through the parser, which 400s a malformed percent-escape first. One that
    // arrives anyway binds the literal bytes it is spelt with: find has no channel to report on, and a decoder trusting
    // the precondition would read past the end rather than merely answer oddly.
    [[nodiscard]] Match find(http::Method method, std::string_view target) const;

    // Every method registered for this path, in registration order, no repeats. Head appears wherever Get does, since
    // the fallback answers it.
    [[nodiscard]] std::vector<http::Method> allowed_methods(std::string_view target) const;

private:
    // Private because nothing outside builds one: add() is the only way in.
    struct Route {
        http::Method method;
        Pattern pattern;
        http::Handler handler;
    };

    std::vector<Route> routes_;
};

}  // namespace carafe::server
