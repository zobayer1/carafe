#pragma once

#include <carafe/http/handler.hpp>
#include <carafe/http/request.hpp>

#include <string>
#include <string_view>
#include <vector>

namespace carafe::server {

// One segment of a path pattern: literal text, or the name a capture binds to.
struct Segment {
    std::string text;
    bool is_param = false;
};

// Split on '/' once, at registration, so matching never re-parses.
using Pattern = std::vector<Segment>;

// Three outcomes, not two: a path registered under a different method owes the
// client a 405 rather than a 404, and a null handler alone cannot say which.
// The handler lives as long as the Router goes unmodified -- add() may
// reallocate, so routes are registered before serving rather than during.
struct Match {
    const http::Handler* handler = nullptr;
    bool path_matched = false;

    // What the pattern captured, empty for a static route. Owned rather than
    // viewed, so nothing here depends on the target outliving the lookup.
    http::Params params;

    [[nodiscard]] explicit operator bool() const noexcept {
        return handler != nullptr;
    }
};

class Router {
public:
    // Appended, so a path registered twice keeps its first handler: find scans in
    // order. That makes a duplicate harmless rather than an error with no channel
    // to report it on.
    void add(http::Method method, std::string_view path, http::Handler handler);

    // The target is expected to have come off the wire through the parser, which
    // rejects a malformed percent-escape with a 400 before this is reached. One
    // that arrives anyway binds the literal bytes it is spelt with: find has no
    // channel to report on, and a decoder written to trust the precondition would
    // read past the end of the target rather than merely answer oddly.
    [[nodiscard]] Match find(http::Method method, std::string_view target) const;

    // Every method registered for this path, in registration order and without
    // repeats. Head appears wherever Get does: the fallback answers it, so a client
    // is entitled to see it listed.
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
