#pragma once

#include <carafe/http/handler.hpp>
#include <carafe/http/request.hpp>

#include <string>
#include <string_view>
#include <vector>

namespace carafe::server {

// Three outcomes, not two: a path registered under a different method owes the
// client a 405 rather than a 404, and a null handler alone cannot say which.
// The handler lives as long as the Router goes unmodified -- add() may
// reallocate, so routes are registered before serving rather than during.
struct Match {
    const http::Handler* handler = nullptr;
    bool path_matched = false;

    [[nodiscard]] explicit operator bool() const noexcept {
        return handler != nullptr;
    }
};

class Router {
public:
    // Appended, so a path registered twice keeps its first handler: find scans in
    // order. That makes a duplicate harmless rather than an error with no channel
    // to report it on.
    void add(http::Method method, std::string path, http::Handler handler);
    [[nodiscard]] Match find(http::Method method, std::string_view target) const;

private:
    // Private because nothing outside builds one: add() is the only way in.
    struct Route {
        http::Method method;
        std::string path;
        http::Handler handler;
    };

    std::vector<Route> routes_;
};

}  // namespace carafe::server
