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
    // Both out of line: Router is incomplete here, so the compiler cannot emit
    // either one where it can see the unique_ptr member.
    App();
    ~App();

    // Neither copied nor moved: nothing needs to, and a moved-from App would
    // still look runnable. Copy is gone with the unique_ptr regardless.
    App(const App&) = delete;
    App& operator=(const App&) = delete;
    App(App&&) = delete;
    App& operator=(App&&) = delete;

    // A path answers only the method it was registered for, and only the first
    // handler given for it. HEAD falls back to the GET registered for the path.
    void get(std::string_view path, http::Handler handler);
    void post(std::string_view path, http::Handler handler);
    void put(std::string_view path, http::Handler handler);
    void patch(std::string_view path, http::Handler handler);

    // Spelt short because `delete` is a keyword.
    void del(std::string_view path, http::Handler handler);

    // The methods without a named helper. Head and Connect are refused with
    // false: HEAD is answered by the Get fallback, whose headers a hand-written
    // route would have to reproduce, and a CONNECT target is an authority rather
    // than a path, so such a route could never match anything.
    [[nodiscard]] bool route(http::Method method, std::string_view path, http::Handler handler);

    // Serves requests on `port`. Returns only on failure -- false if the port
    // could not be bound, or if accepting stopped for a reason retrying would
    // not fix. Routes are registered before this is called, never during.
    [[nodiscard]] bool run(std::uint16_t port);

private:
    std::unique_ptr<server::Router> router_;
};

}  // namespace carafe
