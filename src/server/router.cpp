#include "server/router.hpp"

#include <carafe/http/handler.hpp>
#include <carafe/http/request.hpp>

#include <string>
#include <string_view>
#include <utility>

namespace carafe::server {

namespace {
// The request-target carries the query; routing does not use it. Fragments never
// arrive -- clients strip them before sending -- so '?' is the only cut.
std::string_view path_of(std::string_view target) noexcept {
    return target.substr(0, target.find('?'));
}

}  // namespace

void Router::add(http::Method method, std::string path, http::Handler handler) {
    routes_.push_back({method, std::move(path), std::move(handler)});
}

Match Router::find(http::Method method, std::string_view target) const {
    const std::string_view path = path_of(target);
    const Route* fallback = nullptr;
    bool path_matched = false;

    for (const Route& route : routes_) {
        if (route.path != path) {
            continue;
        }

        path_matched = true;

        // Returned at once, so the first registration of a method wins.
        if (route.method == method) {
            return {&route.handler, true};
        }

        // RFC 9110: HEAD is GET without the body. Remembered rather than returned,
        // so an explicit HEAD route still beats it whenever it was registered.
        if (method == http::Method::Head && route.method == http::Method::Get &&
            fallback == nullptr) {
            fallback = &route;
        }
    }
    if (fallback != nullptr) {
        return {&fallback->handler, true};
    }
    return {nullptr, path_matched};
}

}  // namespace carafe::server
