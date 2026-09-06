#include <carafe/app.hpp>
#include <carafe/config.hpp>
#include <carafe/http/request.hpp>
#include <carafe/http/response.hpp>

#include "net/listener.hpp"

#include <chrono>
#include <cstdint>
#include <string_view>

#include <gtest/gtest.h>

namespace {

using carafe::App;
using carafe::Deadlines;
using carafe::describe;
using carafe::PoolLimits;
using carafe::RunError;
using carafe::http::Method;
using carafe::http::Request;
using carafe::http::text_response;

// Registration is all that can be observed here: App exposes no way to look a route up, and a run that reaches its
// accept loop never returns.
carafe::http::Handler noop() {
    return [](const Request&) { return text_response(200, ""); };
}

// The verbs route() exists for, having no named helper of their own.
TEST(App, RegistersTheMethodsWithoutANamedHelper) {
    App app;
    EXPECT_TRUE(app.route(Method::Options, "/a", noop()));
    EXPECT_TRUE(app.route(Method::Trace, "/b", noop()));
}

// route() is a superset of the helpers rather than a leftovers bin.
TEST(App, RegistersTheMethodsThatHaveNamedHelpers) {
    App app;
    EXPECT_TRUE(app.route(Method::Get, "/a", noop()));
    EXPECT_TRUE(app.route(Method::Post, "/b", noop()));
    EXPECT_TRUE(app.route(Method::Put, "/c", noop()));
    EXPECT_TRUE(app.route(Method::Patch, "/d", noop()));
    EXPECT_TRUE(app.route(Method::Delete, "/e", noop()));
}

// HEAD is answered by the Get fallback, whose headers a hand-written route would have to reproduce; a CONNECT target is
// an authority rather than a path, so such a route could never match. Neither has a named helper, and route() is the
// only other way in.
TEST(App, RefusesTheMethodsItCannotHonour) {
    App app;
    EXPECT_FALSE(app.route(Method::Head, "/a", noop()));
    EXPECT_FALSE(app.route(Method::Connect, "/b", noop()));
}

// Zero is reachable now that the numbers come from outside, and each of these serves nobody: no worker ever takes from
// the queue, an empty queue closes every arrival even while the workers sit idle, and a zero wait drops whatever it
// queued. The port is held throughout, so a check that had gone missing would report a failed bind instead.
TEST(App, RefusesAPoolThatWouldServeNobody) {
    auto held = carafe::net::listen_on(0);
    ASSERT_TRUE(held.listener.has_value());
    const std::uint16_t port = held.listener->port();

    App app;
    EXPECT_EQ(app.run(port, PoolLimits{0, 512, std::chrono::seconds(5)}), RunError::InvalidLimits);
    EXPECT_EQ(app.run(port, PoolLimits{64, 0, std::chrono::seconds(5)}), RunError::InvalidLimits);
    EXPECT_EQ(app.run(port, PoolLimits{64, 512, std::chrono::milliseconds(0)}), RunError::InvalidLimits);
}

// A zero deadline reaches setsockopt as no deadline at all, so honouring it would hand back the unbounded wait the
// deadlines exist to remove.
TEST(App, RefusesADeadlineOfZero) {
    auto held = carafe::net::listen_on(0);
    ASSERT_TRUE(held.listener.has_value());
    const std::uint16_t port = held.listener->port();

    App app;
    EXPECT_EQ(app.run(port, PoolLimits{}, Deadlines{std::chrono::milliseconds(0), std::chrono::seconds(30)}),
              RunError::InvalidDeadlines);
    EXPECT_EQ(app.run(port, PoolLimits{}, Deadlines{std::chrono::seconds(30), std::chrono::milliseconds(0)}),
              RunError::InvalidDeadlines);
}

// The checks sit in front of the bind, so a configuration with nothing wrong in it must still reach that bind and
// report it failing. Otherwise the two tests above could not tell a refusal from a check that never ran.
TEST(App, ReportsAPortItCannotBind) {
    auto held = carafe::net::listen_on(0);
    ASSERT_TRUE(held.listener.has_value());

    App app;
    EXPECT_EQ(app.run(held.listener->port()), RunError::BindFailed);
}

// Every failure carries its own words, so a caller printing one cannot blame the port for a bad deadline.
TEST(App, DescribesEachFailureDifferently) {
    const std::string_view limits = describe(RunError::InvalidLimits);
    const std::string_view deadlines = describe(RunError::InvalidDeadlines);
    const std::string_view bind = describe(RunError::BindFailed);
    const std::string_view accept = describe(RunError::AcceptFailed);

    EXPECT_FALSE(limits.empty());
    EXPECT_NE(limits, deadlines);
    EXPECT_NE(deadlines, bind);
    EXPECT_NE(bind, accept);
}

}  // namespace
