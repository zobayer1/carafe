#include "server/router.hpp"

#include <carafe/http/handler.hpp>
#include <carafe/http/request.hpp>
#include <carafe/http/response.hpp>

#include <string>
#include <utility>

#include <gtest/gtest.h>

namespace {

using carafe::http::Handler;
using carafe::http::Method;
using carafe::http::Request;
using carafe::http::Response;
using carafe::server::Match;
using carafe::server::Router;

// Handlers are told apart by what they answer: a non-null pointer proves only
// that something matched, not that it was the route we meant.
Handler answering(std::string tag) {
    return [tag = std::move(tag)](const Request&) {
        Response response;
        response.body = tag;
        return response;
    };
}

std::string answer_of(const Match& match) {
    return match.handler == nullptr ? std::string{} : (*match.handler)(Request{}).body;
}

TEST(Router, FindsNothingWhenNothingIsRegistered) {
    const Router router;

    const Match match = router.find(Method::Get, "/hello");

    EXPECT_FALSE(match);
    EXPECT_FALSE(match.path_matched);
}

TEST(Router, FindsTheHandlerRegisteredForTheMethodAndPath) {
    Router router;
    router.add(Method::Get, "/hello", answering("hello"));

    const Match match = router.find(Method::Get, "/hello");

    EXPECT_TRUE(match);
    EXPECT_TRUE(match.path_matched);
    EXPECT_EQ(answer_of(match), "hello");
}

// The two ways of not matching, and they are not interchangeable: one owes the
// client a 404 and the other a 405.
TEST(Router, ReportsAnUnregisteredPathAsNoPathAtAll) {
    Router router;
    router.add(Method::Get, "/hello", answering("hello"));

    const Match match = router.find(Method::Get, "/elsewhere");

    EXPECT_FALSE(match);
    EXPECT_FALSE(match.path_matched);
}

TEST(Router, ReportsAKnownPathUnderTheWrongMethod) {
    Router router;
    router.add(Method::Post, "/hello", answering("hello"));

    const Match match = router.find(Method::Put, "/hello");

    EXPECT_FALSE(match);
    EXPECT_TRUE(match.path_matched);
}

// Routing is by path, and the target that arrives carries the query with it.
TEST(Router, IgnoresTheQueryString) {
    Router router;
    router.add(Method::Get, "/hello", answering("hello"));

    EXPECT_EQ(answer_of(router.find(Method::Get, "/hello?name=x")), "hello");
    EXPECT_EQ(answer_of(router.find(Method::Get, "/hello?")), "hello");
    EXPECT_EQ(answer_of(router.find(Method::Get, "/hello?a=1?b=2")), "hello");
}

// A question mark cuts, it does not match: a route may not be reached by asking
// for a longer path with a query bolted on.
TEST(Router, DoesNotLetAQueryStringStandInForThePath) {
    Router router;
    router.add(Method::Get, "/hello", answering("hello"));

    EXPECT_FALSE(router.find(Method::Get, "/hello/more?x=1"));
    EXPECT_FALSE(router.find(Method::Get, "/?/hello"));
}

// RFC 9110 defines HEAD as GET without the body, so registering GET answers both.
TEST(Router, HeadFindsTheGetRoute) {
    Router router;
    router.add(Method::Get, "/hello", answering("get"));

    const Match match = router.find(Method::Head, "/hello");

    EXPECT_TRUE(match);
    EXPECT_EQ(answer_of(match), "get");
}

// The fallback runs one way only. A GET must not be answered by a handler
// written for HEAD, which is entitled to send nothing at all.
TEST(Router, GetDoesNotFallBackToAHeadRoute) {
    Router router;
    router.add(Method::Head, "/hello", answering("head"));

    const Match match = router.find(Method::Get, "/hello");

    EXPECT_FALSE(match);
    EXPECT_TRUE(match.path_matched);
}

// Remembered rather than returned, so registration order cannot decide this.
TEST(Router, AnExplicitHeadRouteBeatsTheGetFallbackInEitherOrder) {
    Router before;
    before.add(Method::Head, "/hello", answering("head"));
    before.add(Method::Get, "/hello", answering("get"));

    Router after;
    after.add(Method::Get, "/hello", answering("get"));
    after.add(Method::Head, "/hello", answering("head"));

    EXPECT_EQ(answer_of(before.find(Method::Head, "/hello")), "head");
    EXPECT_EQ(answer_of(after.find(Method::Head, "/hello")), "head");
}

TEST(Router, HeadFallsBackOnlyWhenThePathHasAGetRoute) {
    Router router;
    router.add(Method::Post, "/hello", answering("post"));

    const Match match = router.find(Method::Head, "/hello");

    EXPECT_FALSE(match);
    EXPECT_TRUE(match.path_matched);
}

// Registering twice is harmless rather than an error, and this is the rule that
// makes it so.
TEST(Router, KeepsTheFirstHandlerRegisteredForAPath) {
    Router router;
    router.add(Method::Get, "/hello", answering("first"));
    router.add(Method::Get, "/hello", answering("second"));

    EXPECT_EQ(answer_of(router.find(Method::Get, "/hello")), "first");
}

// First-registration-wins has to hold for the fallback too, and nothing else
// pins it: the exact-match tests return before a fallback is ever considered.
TEST(Router, KeepsTheFirstGetRouteWhenHeadFallsBack) {
    Router router;
    router.add(Method::Get, "/hello", answering("first"));
    router.add(Method::Get, "/hello", answering("second"));

    EXPECT_EQ(answer_of(router.find(Method::Head, "/hello")), "first");
}

TEST(Router, MatchesPathsExactly) {
    Router router;
    router.add(Method::Get, "/hello", answering("hello"));

    EXPECT_FALSE(router.find(Method::Get, "/hello/"));
    EXPECT_FALSE(router.find(Method::Get, "/Hello"));
    EXPECT_FALSE(router.find(Method::Get, "/hell"));
    EXPECT_FALSE(router.find(Method::Get, ""));
}

// Several routes at once, since one-route tests cannot tell a scan from a
// function that returns whatever it was last handed.
TEST(Router, KeepsSeparateRoutesApart) {
    Router router;
    router.add(Method::Get, "/one", answering("one"));
    router.add(Method::Post, "/one", answering("one-post"));
    router.add(Method::Get, "/two", answering("two"));

    EXPECT_EQ(answer_of(router.find(Method::Get, "/one")), "one");
    EXPECT_EQ(answer_of(router.find(Method::Post, "/one")), "one-post");
    EXPECT_EQ(answer_of(router.find(Method::Get, "/two")), "two");
    EXPECT_FALSE(router.find(Method::Post, "/two"));
}

// The handler is called with the request the caller passes, not with the one it
// was registered against -- there is no such thing.
TEST(Router, PassesTheRequestThroughToTheHandler) {
    Router router;
    router.add(Method::Get, "/echo", [](const Request& request) {
        Response response;
        response.body = request.target;
        return response;
    });

    Request request;
    request.method = Method::Get;
    request.target = "/echo?q=1";

    const Match match = router.find(request.method, request.target);

    ASSERT_TRUE(match);
    EXPECT_EQ((*match.handler)(request).body, "/echo?q=1");
}

}  // namespace
