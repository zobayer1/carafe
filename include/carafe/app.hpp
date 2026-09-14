#pragma once

#include <carafe/config.hpp>
#include <carafe/http/handler.hpp>
#include <carafe/http/request.hpp>

#include <cstdint>
#include <memory>
#include <string_view>

// Forward declaration for Pipeline defined in src/server/pipeline.hpp file.
namespace carafe::server {
class Pipeline;
}

namespace carafe {

// Why `run` gave up. Every value is a failure: it has no success to report, since serving forever is what it does
// until something stops it. No `None`, so a caller cannot mistake one for a run that is still going.
enum class RunError {
    InvalidLimits,
    InvalidDeadlines,
    BindFailed,
    AcceptFailed,
};

// A line fit to print. One wording per failure, in the library rather than in each caller.
[[nodiscard]] std::string_view describe(RunError error) noexcept;

class App {
public:
    // Out of line: make_shared needs Pipeline complete, and this header only forward-declares it.
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

    // Returns only on failure, and says which one. Register routes before calling, not during.
    //
    // Zero is refused rather than honoured: no workers and no queue room each serve nobody, and a zero deadline
    // reaches the socket as no deadline at all. Ask for no limit with a large value instead.
    [[nodiscard]] RunError run(std::uint16_t port, PoolLimits limits = {}, Deadlines deadlines = {});

private:
    std::shared_ptr<server::Pipeline> pipeline_;
};

}  // namespace carafe
