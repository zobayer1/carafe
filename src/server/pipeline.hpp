#pragma once

#include <carafe/http/handler.hpp>
#include <carafe/http/request.hpp>
#include <carafe/http/response.hpp>

#include "server/router.hpp"

#include <string_view>

namespace carafe::server {

// What the application does with a parsed request: route it, then run the handler or say why none ran. Every worker
// calls respond() on one shared instance at once, so nothing may change it once serving has started.
class Pipeline {
public:
    // Forwarded to the Router, with its rules: the first registration wins, and registration comes before serving.
    void add(http::Method method, std::string_view path, http::Handler handler);

    // What the application answers a parsed request with: the route's handler, a 404 or 405 when no route claims it,
    // or a 500 when the handler throws. It binds the route's captures into `request.params` on the way, which is why
    // the request is not const. Whether to close, and whether HEAD drops the body, stay with the caller.
    [[nodiscard]] http::Response respond(http::Request& request) const;

private:
    Router router_;
};

}  // namespace carafe::server
