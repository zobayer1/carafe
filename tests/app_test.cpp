#include <carafe/app.hpp>
#include <carafe/http/request.hpp>
#include <carafe/http/response.hpp>

#include <gtest/gtest.h>

namespace {

using carafe::App;
using carafe::http::Method;
using carafe::http::Request;
using carafe::http::text_response;

// Registration is all that can be observed here: App exposes no way to look a
// route up, and run() never returns. The bool is the whole contract.
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

// HEAD is answered by the Get fallback, whose headers a hand-written route would
// have to reproduce; a CONNECT target is an authority rather than a path, so such
// a route could never match. Neither has a named helper, and route() is the only
// other way in.
TEST(App, RefusesTheMethodsItCannotHonour) {
    App app;
    EXPECT_FALSE(app.route(Method::Head, "/a", noop()));
    EXPECT_FALSE(app.route(Method::Connect, "/b", noop()));
}

}  // namespace
