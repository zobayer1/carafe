#include "server/pipeline.hpp"

#include <carafe/http/handler.hpp>
#include <carafe/http/middleware.hpp>
#include <carafe/http/request.hpp>
#include <carafe/http/response.hpp>

#include "server/router.hpp"

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace carafe::server {

// One request's walk through the chain, built on the stack in respond and pointed at by every Next it hands out.
struct Exchange {
    const Router* router;
    const std::vector<http::Middleware>* chain;
    const Match* match;
    const http::Request* original;
};

namespace {

// Comma-separated, as RFC 9110 spells the field. Method names are case-sensitive tokens, so these stay uppercase though
// every field name we emit is lowered.
[[nodiscard]] std::string allow_value(const std::vector<http::Method>& methods) {
    std::string value;
    for (const http::Method method : methods) {
        if (!value.empty()) {
            value += ", ";
        }
        value += http::method_name(method);
    }
    return value;
}

// A path nobody registered is a 404; one registered under another method is a 405, and RFC 9110 makes Allow on that 405
// a MUST rather than a courtesy.
[[nodiscard]] http::Response unmatched_response(const Router& router, std::string_view target, bool path_matched) {
    if (!path_matched) {
        return http::status_response(404);
    }
    http::Response response = http::status_response(405);
    response.headers.add({"allow", allow_value(router.allowed_methods(target))});
    return response;
}

// A handler is the caller's code, so whatever it throws is this request's failure and no one else's.
[[nodiscard]] http::Response run_handler(const http::Handler& handler, const http::Request& request) {
    try {
        return handler(request);
    } catch (...) {
        return http::status_response(500);
    }
}

// Past the last middleware is dispatch. The Allow list on a 405 comes from the original target, since that is the path
// the route was chosen from, whatever a middleware handed on.
[[nodiscard]] http::Response step(const Exchange& exchange, std::size_t index, const http::Request& request) {
    if (index == exchange.chain->size()) {
        return *exchange.match
                   ? run_handler(*exchange.match->handler, request)
                   : unmatched_response(*exchange.router, exchange.original->target, exchange.match->path_matched);
    }
    return (*exchange.chain)[index](request, http::Next{exchange, index + 1});
}

}  // namespace

void Pipeline::add(http::Method method, std::string_view path, http::Handler handler) {
    router_.add(method, path, std::move(handler));
}

void Pipeline::use(http::Middleware middleware) {
    middlewares_.emplace_back(std::move(middleware));
}

http::Response Pipeline::respond(http::Request& request) const {
    auto match = router_.find(request.method, request.target);

    // Unconditional: an unmatched request captured nothing, so this costs an empty vector rather than a branch.
    request.params = std::move(match.params);

    const Exchange exchange{&router_, &middlewares_, &match, &request};

    // Middleware is the caller's code as much as a handler is, so a throw from anywhere in the chain is this request's
    // 500. The handler's own catch stays, which is what lets an outer middleware see that 500 as a response.
    try {
        return step(exchange, 0, request);
    } catch (...) {
        return http::status_response(500);
    }
}

}  // namespace carafe::server

namespace carafe::http {

Response Next::operator()(const Request& request) const {
    return server::step(*exchange_, index_, request);
}

}  // namespace carafe::http
