#include "server/pipeline.hpp"

#include <carafe/http/handler.hpp>
#include <carafe/http/request.hpp>
#include <carafe/http/response.hpp>

#include "server/router.hpp"

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace carafe::server {

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

}  // namespace

void Pipeline::add(http::Method method, std::string_view path, http::Handler handler) {
    router_.add(method, path, std::move(handler));
}

http::Response Pipeline::respond(http::Request& request) const {
    auto match = router_.find(request.method, request.target);

    // Unconditional: an unmatched request captured nothing, so this costs an empty vector rather than a branch.
    request.params = std::move(match.params);

    return match ? run_handler(*match.handler, request)
                 : unmatched_response(router_, request.target, match.path_matched);
}

}  // namespace carafe::server
