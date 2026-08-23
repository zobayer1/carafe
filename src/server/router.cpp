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
// The request-target carries the query; routing does not use it. Fragments never
// arrive -- clients strip them before sending -- so '?' is the only cut.
std::string_view path_of(std::string_view target) noexcept {
    return target.substr(0, target.find('?'));
}

// One definition of what "this route serves this path" means, because find()
// and allowed_methods() must never disagree about it.
bool matches(std::string_view pattern, std::string_view path) noexcept {
    return pattern == path;
}

bool contains(const std::vector<http::Method>& methods, http::Method method) {
    return std::find(methods.begin(), methods.end(), method) != methods.end();
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
        if (!matches(route.path, path)) {
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

std::vector<http::Method> Router::allowed_methods(std::string_view target) const {
    const std::string_view path = path_of(target);
    std::vector<http::Method> allowed;

    for (const Route& route : routes_) {
        if (!matches(route.path, path)) {
            continue;
        }

        if (!contains(allowed, route.method)) {
            allowed.push_back(route.method);
        }

        // Listed beside the Get that implies it rather than appended, so the order
        // still reads as the order the routes were registered in.
        if (route.method == http::Method::Get && !contains(allowed, http::Method::Head)) {
            allowed.push_back(http::Method::Head);
        }
    }
    return allowed;
}

}  // namespace carafe::server
