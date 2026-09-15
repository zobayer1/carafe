#pragma once

#include <carafe/http/handler.hpp>
#include <carafe/http/middleware.hpp>
#include <carafe/http/request.hpp>
#include <carafe/http/response.hpp>

#include "server/router.hpp"

#include <string_view>
#include <vector>

namespace carafe::server {

// What the application does with a parsed request: route it, then pass it through the middleware to the handler, or to
// the 404 or 405 when no route claims it. Every worker calls respond() on one shared instance at once, so nothing may
// change it once serving has started.
class Pipeline {
public:
    // Forwarded to the Router, whose rule stands: the first registration for a path wins.
    void add(http::Method method, std::string_view path, http::Handler handler);

    // The first registered runs outermost, and each wraps every request respond is given, matched or not.
    void use(http::Middleware middleware);

    // Whatever the middleware and the route's handler make of the request, a 404 or 405 when no route claims it, or a
    // 500 when a handler or a middleware throws. The captures are bound into `request.params` before the middleware
    // runs, which is why the request is not const. Whether to close, and whether HEAD drops the body, stay with the
    // caller.
    [[nodiscard]] http::Response respond(http::Request& request) const;

private:
    Router router_;
    std::vector<http::Middleware> middlewares_;
};

}  // namespace carafe::server
