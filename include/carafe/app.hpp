#pragma once

#include <carafe/http/handler.hpp>
#include <carafe/http/request.hpp>

#include <cstdint>
#include <memory>
#include <string_view>

namespace carafe::server {
class Router;
}

namespace carafe {

class App {
public:
    // Out of line: make_shared needs Router complete, and this header only forward-declares it.
    App();

    ~App() = default;

    // A moved-from App would still look runnable.
    App(const App&) = delete;
    App& operator=(const App&) = delete;
    App(App&&) = delete;
    App& operator=(App&&) = delete;

    // A path answers only the method it was registered for, and only the first handler given for it. HEAD falls back to
    // the path's GET.
    void get(std::string_view path, http::Handler handler);
    void post(std::string_view path, http::Handler handler);
    void put(std::string_view path, http::Handler handler);
    void patch(std::string_view path, http::Handler handler);

    // `delete` is a keyword.
    void del(std::string_view path, http::Handler handler);

    // The methods with no named helper. False for Head and Connect: HEAD is answered by the Get fallback, whose headers
    // a hand-written route would have to reproduce, and a CONNECT target is an authority rather than a path.
    [[nodiscard]] bool route(http::Method method, std::string_view path, http::Handler handler);

    // Returns only on failure: the port would not bind, or accepting stopped for a reason retrying would not fix.
    // Register routes before calling, not during.
    [[nodiscard]] bool run(std::uint16_t port);

private:
    std::shared_ptr<server::Router> router_;
};

}  // namespace carafe
