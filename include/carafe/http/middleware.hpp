#pragma once

#include <carafe/http/request.hpp>
#include <carafe/http/response.hpp>

#include <cstddef>
#include <functional>

namespace carafe::server {
struct Exchange;
}

namespace carafe::http {

// The rest of the chain, and the handler at its end. Calling it runs everything after this middleware and returns what
// that answered; returning without calling it answers in their place. Every call runs the rest again, handler included.
// Valid only during the call it was handed to. The route was chosen before the chain began, so a request passed here,
// even a modified copy, reaches that route.
class Next {
public:
    // Only the pipeline can build an Exchange, so nothing outside can build a Next.
    Next(const server::Exchange& exchange, std::size_t index) noexcept : exchange_(&exchange), index_(index) {}

    [[nodiscard]] Response operator()(const Request& request) const;

private:
    const server::Exchange* exchange_;
    std::size_t index_;
};

// Runs around every request that parsed, 404 and 405 included, and is called from every worker at once, so a middleware
// that keeps state guards it. It sees the target as the client sent it, not the normalised path the router matched, so
// it must not decide anything by path prefix: "/x/../admin" is routed to "/admin".
using Middleware = std::function<Response(const Request&, const Next&)>;

}  // namespace carafe::http
