#include "server/router.hpp"

#include <carafe/http/handler.hpp>
#include <carafe/http/request.hpp>
#include <carafe/http/response.hpp>

#include "http/printers.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

namespace {

using carafe::http::Handler;
using carafe::http::Method;
using carafe::http::Request;
using carafe::http::Response;
using carafe::server::Match;
using carafe::server::Router;

// Handlers are told apart by what they answer: a non-null pointer proves only that something matched, not that it was
// the route we meant.
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

// find answers by value and get() views into it, so the Match has to outlive the view: this hands back an owned copy
// instead. Nothing bound stays distinct from an empty string bound, because those mean different things.
std::optional<std::string> bound_param(const Router& router, std::string_view target, std::string_view name) {
    const Match match = router.find(Method::Get, target);
    const std::optional<std::string_view> value = match.params.get(name);
    if (!value.has_value()) {
        return std::nullopt;
    }
    return std::string{*value};
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

// The two ways of not matching, and they are not interchangeable: one owes the client a 404 and the other a 405.
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

// A question mark cuts, it does not match: a route may not be reached by asking for a longer path with a query bolted
// on.
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

// The fallback runs one way only. A GET must not be answered by a handler written for HEAD, which is entitled to send
// nothing at all.
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

// Registering twice is harmless rather than an error, and this is the rule that makes it so.
TEST(Router, KeepsTheFirstHandlerRegisteredForAPath) {
    Router router;
    router.add(Method::Get, "/hello", answering("first"));
    router.add(Method::Get, "/hello", answering("second"));

    EXPECT_EQ(answer_of(router.find(Method::Get, "/hello")), "first");
}

// First-registration-wins has to hold for the fallback too, and nothing else pins it: the exact-match tests return
// before a fallback is ever considered.
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

// Several routes at once, since one-route tests cannot tell a scan from a function that returns whatever it was last
// handed.
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

// The handler is called with the request the caller passes, not with the one it was registered against: there is no
// such thing.
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

// The 405 owes the client an Allow header, and these are the methods that go in it. Compared as a whole vector, because
// order and repetition are both part of what the field says.
using Methods = std::vector<Method>;

TEST(Router, AllowsNothingForAPathItDoesNotServe) {
    Router router;
    router.add(Method::Get, "/hello", answering("hello"));

    EXPECT_EQ(router.allowed_methods("/missing"), Methods{});
    EXPECT_EQ(Router{}.allowed_methods("/hello"), Methods{});
}

// One method registered means one method allowed: nothing is assumed for a path just because it exists.
TEST(Router, AllowsOnlyTheMethodRegistered) {
    Router router;
    router.add(Method::Post, "/submit", answering("submit"));

    EXPECT_EQ(router.allowed_methods("/submit"), Methods{Method::Post});
}

// find() answers HEAD from a GET route, so Allow has to say so. Otherwise the field contradicts what the next request
// would actually get.
TEST(Router, AllowsHeadWhereverGetIsAllowed) {
    Router router;
    router.add(Method::Get, "/hello", answering("hello"));

    EXPECT_EQ(router.allowed_methods("/hello"), (Methods{Method::Get, Method::Head}));
}

// Head follows the Get that implies it rather than trailing the list, so the order still reads as the order the routes
// were registered in.
TEST(Router, ListsMethodsInRegistrationOrder) {
    Router router;
    router.add(Method::Get, "/thing", answering("get"));
    router.add(Method::Post, "/thing", answering("post"));
    router.add(Method::Delete, "/thing", answering("delete"));

    EXPECT_EQ(router.allowed_methods("/thing"), (Methods{Method::Get, Method::Head, Method::Post, Method::Delete}));
}

TEST(Router, ListsAMethodOnceHoweverOftenItIsRegistered) {
    Router router;
    router.add(Method::Get, "/hello", answering("first"));
    router.add(Method::Get, "/hello", answering("second"));

    EXPECT_EQ(router.allowed_methods("/hello"), (Methods{Method::Get, Method::Head}));
}

// Two ways to arrive at Head, the Get fallback and an explicit route, and they must not both put it in the list.
TEST(Router, ListsHeadOnceWhenItIsRegisteredAlongsideGet) {
    Router before;
    before.add(Method::Get, "/hello", answering("get"));
    before.add(Method::Head, "/hello", answering("head"));

    Router after;
    after.add(Method::Head, "/hello", answering("head"));
    after.add(Method::Get, "/hello", answering("get"));

    EXPECT_EQ(before.allowed_methods("/hello"), (Methods{Method::Get, Method::Head}));
    EXPECT_EQ(after.allowed_methods("/hello"), (Methods{Method::Head, Method::Get}));
}

// Head is GET without the body and nothing else: a bodiless POST is not a thing the router may invent.
TEST(Router, DoesNotAllowHeadForMethodsOtherThanGet) {
    Router router;
    router.add(Method::Post, "/a", answering("post"));
    router.add(Method::Put, "/b", answering("put"));
    router.add(Method::Delete, "/c", answering("delete"));

    EXPECT_EQ(router.allowed_methods("/a"), Methods{Method::Post});
    EXPECT_EQ(router.allowed_methods("/b"), Methods{Method::Put});
    EXPECT_EQ(router.allowed_methods("/c"), Methods{Method::Delete});
}

TEST(Router, AllowsPerPathRatherThanAcrossTheTable) {
    Router router;
    router.add(Method::Get, "/one", answering("one"));
    router.add(Method::Post, "/two", answering("two"));

    EXPECT_EQ(router.allowed_methods("/one"), (Methods{Method::Get, Method::Head}));
    EXPECT_EQ(router.allowed_methods("/two"), Methods{Method::Post});
}

// The same path rule find() uses, since a set of methods for a path find() would never reach is worse than none at all.
TEST(Router, AllowsAgainstTheSamePathFindMatches) {
    Router router;
    router.add(Method::Post, "/hello", answering("post"));

    EXPECT_EQ(router.allowed_methods("/hello?name=x"), Methods{Method::Post});
    EXPECT_EQ(router.allowed_methods("/hello/"), Methods{});
    EXPECT_EQ(router.allowed_methods("/Hello"), Methods{});
    EXPECT_EQ(router.allowed_methods("/hello/more?x=1"), Methods{});
}

// Path parameters. A pattern segment written <name> stands for any one segment and remembers what stood there;
// everything above still has to hold unchanged.

TEST(Router, FindsAHandlerThroughAPathParameter) {
    Router router;
    router.add(Method::Get, "/users/<id>", answering("user"));

    const Match match = router.find(Method::Get, "/users/42");

    EXPECT_TRUE(match);
    EXPECT_TRUE(match.path_matched);
    EXPECT_EQ(answer_of(match), "user");
}

TEST(Router, CapturesTheSegmentAParameterStoodFor) {
    Router router;
    router.add(Method::Get, "/users/<id>", answering("user"));

    const Match match = router.find(Method::Get, "/users/42");

    ASSERT_TRUE(match);
    EXPECT_EQ(match.params.get("id"), "42");
}

// The name comes from the pattern, not from anything the client sent, so asking for the literal text of the segment
// finds nothing.
TEST(Router, CapturesUnderTheNameThePatternGave) {
    Router router;
    router.add(Method::Get, "/users/<id>", answering("user"));

    const Match match = router.find(Method::Get, "/users/42");

    ASSERT_TRUE(match);
    EXPECT_FALSE(match.params.get("42").has_value());
    EXPECT_FALSE(match.params.get("users").has_value());
}

TEST(Router, CapturesEveryParameterInThePattern) {
    Router router;
    router.add(Method::Get, "/users/<id>/posts/<slug>", answering("post"));

    const Match match = router.find(Method::Get, "/users/7/posts/hello");

    ASSERT_TRUE(match);
    EXPECT_EQ(match.params.get("id"), "7");
    EXPECT_EQ(match.params.get("slug"), "hello");
    EXPECT_EQ(match.params.entries.size(), 2U);
}

TEST(Router, CapturesNothingForAStaticRoute) {
    Router router;
    router.add(Method::Get, "/hello", answering("hello"));

    const Match match = router.find(Method::Get, "/hello");

    ASSERT_TRUE(match);
    EXPECT_TRUE(match.params.entries.empty());
}

// A parameter binds one segment, not a tail: the slash is a boundary it cannot reach across, in either direction.
TEST(Router, BindsAParameterToOneSegmentOnly) {
    Router router;
    router.add(Method::Get, "/users/<id>", answering("user"));

    EXPECT_FALSE(router.find(Method::Get, "/users/42/posts"));
    EXPECT_FALSE(router.find(Method::Get, "/users"));
    EXPECT_FALSE(router.find(Method::Get, "/42"));
}

// Nothing is not a value, so a parameter refuses an empty segment rather than binding an empty string a handler could
// not tell from a missing one.
TEST(Router, DoesNotBindAParameterToAnEmptySegment) {
    Router router;
    router.add(Method::Get, "/users/<id>", answering("user"));

    EXPECT_FALSE(router.find(Method::Get, "/users/"));
    EXPECT_FALSE(router.find(Method::Get, "/users//"));
}

TEST(Router, MatchesAParameterInTheMiddleOfAPath) {
    Router router;
    router.add(Method::Get, "/users/<id>/edit", answering("edit"));

    EXPECT_EQ(answer_of(router.find(Method::Get, "/users/42/edit")), "edit");
    EXPECT_FALSE(router.find(Method::Get, "/users/42/delete"));
}

// The query is cut before matching, so it can neither be captured nor make a parameterised route match a path it does
// not describe.
TEST(Router, CapturesThePathWithoutTheQuery) {
    Router router;
    router.add(Method::Get, "/users/<id>", answering("user"));

    const Match match = router.find(Method::Get, "/users/42?fields=name");

    ASSERT_TRUE(match);
    EXPECT_EQ(match.params.get("id"), "42");
}

// Registration order decides, exactly as it does for two identical static paths. A literal route must be registered
// before the pattern that swallows it.
TEST(Router, TakesTheFirstPatternThatMatches) {
    Router literal_first;
    literal_first.add(Method::Get, "/users/me", answering("me"));
    literal_first.add(Method::Get, "/users/<id>", answering("by-id"));

    Router pattern_first;
    pattern_first.add(Method::Get, "/users/<id>", answering("by-id"));
    pattern_first.add(Method::Get, "/users/me", answering("me"));

    EXPECT_EQ(answer_of(literal_first.find(Method::Get, "/users/me")), "me");
    EXPECT_EQ(answer_of(literal_first.find(Method::Get, "/users/42")), "by-id");
    EXPECT_EQ(answer_of(pattern_first.find(Method::Get, "/users/me")), "by-id");
}

// A parameter with no name is one no handler could ever ask for, so <> stays the literal text it looks like rather than
// becoming a capture.
TEST(Router, TreatsAnEmptyParameterNameAsLiteralText) {
    Router router;
    router.add(Method::Get, "/users/<>", answering("literal"));

    EXPECT_EQ(answer_of(router.find(Method::Get, "/users/<>")), "literal");
    EXPECT_FALSE(router.find(Method::Get, "/users/42"));
}

// Both brackets are required, so a segment carrying only one of them is text. Neither character can arrive in a real
// request target, since RFC 3986 excludes them from a path, which is exactly why they are safe to spell patterns with.
TEST(Router, TreatsAHalfBracketedSegmentAsLiteralText) {
    Router router;
    router.add(Method::Get, "/users/<id", answering("open"));
    router.add(Method::Get, "/users/id>", answering("close"));

    EXPECT_EQ(answer_of(router.find(Method::Get, "/users/<id")), "open");
    EXPECT_EQ(answer_of(router.find(Method::Get, "/users/id>")), "close");
    EXPECT_FALSE(router.find(Method::Get, "/users/42"));
}

// Odd rather than wrong, and add() has no channel to refuse it on. Both are captured and get() answers with the first,
// as it does for repeated headers.
TEST(Router, CapturesBothWhenAPatternBindsOneNameTwice) {
    Router router;
    router.add(Method::Get, "/pair/<id>/<id>", answering("pair"));

    const Match match = router.find(Method::Get, "/pair/first/second");

    ASSERT_TRUE(match);
    EXPECT_EQ(match.params.entries.size(), 2U);
    EXPECT_EQ(match.params.get("id"), "first");
}

// The fallback is remembered a pass of the loop before it is returned, and the captures have to be remembered with it:
// they cannot be recomputed later. A capture is what the client meant, not the spelling it travelled in: RFC 3986 2.1
// makes "%20" a space, and 6.2.2.1 makes both hex cases the same byte.
TEST(Router, DecodesPercentEscapesInACapture) {
    Router router;
    router.add(Method::Get, "/users/<id>", answering("user"));

    EXPECT_EQ(bound_param(router, "/users/a%20b", "id"), "a b");
    EXPECT_EQ(bound_param(router, "/users/caf%C3%A9", "id"), "caf\xC3\xA9");
    EXPECT_EQ(bound_param(router, "/users/%2f%2F", "id"), "//");
}

// The split runs first and the decode second, so an escaped slash is a byte of one segment rather than a separator
// between two. The other order is how a path-traversal bug gets in, and forbidding it is what this test is for.
TEST(Router, DecodesAnEscapedSlashWithoutSplittingOnIt) {
    Router router;
    router.add(Method::Get, "/users/<id>", answering("user"));

    const Match match = router.find(Method::Get, "/users/a%2Fb");

    ASSERT_TRUE(match);
    ASSERT_EQ(match.params.entries.size(), 1U);
    EXPECT_EQ(match.params.entries.front().value, "a/b");
}

// path_of cuts on a raw '?' before anything is decoded, so an escaped one stays inside the segment instead of ending
// the path early.
TEST(Router, DecodesAnEscapedQuestionMarkWithoutCuttingTheQuery) {
    Router router;
    router.add(Method::Get, "/users/<id>", answering("user"));

    EXPECT_EQ(bound_param(router, "/users/a%3Fb", "id"), "a?b");
}

// '+' stands for a space in application/x-www-form-urlencoded, which is a rule about query strings and form bodies. A
// path segment is neither.
TEST(Router, DoesNotTreatPlusAsASpace) {
    Router router;
    router.add(Method::Get, "/users/<id>", answering("user"));

    EXPECT_EQ(bound_param(router, "/users/a+b", "id"), "a+b");
}

// Unreachable through a served request, since the parser answers all three with a 400. But find promises nothing about
// its target, and the last two are where a decoder that trusted the parser would read past the end of one.
TEST(Router, CopiesAnUndecodableEscapeLiterally) {
    Router router;
    router.add(Method::Get, "/users/<id>", answering("user"));

    EXPECT_EQ(bound_param(router, "/users/a%zzb", "id"), "a%zzb");
    EXPECT_EQ(bound_param(router, "/users/a%4", "id"), "a%4");
    EXPECT_EQ(bound_param(router, "/users/%", "id"), "%");
}

// A known gap, not a decision: only captures are decoded, so a literal segment is still compared as the bytes it was
// registered with. Expected to fail the day a pattern and a request path are normalised against each other.
TEST(Router, DoesNotDecodeLiteralSegments) {
    Router router;
    router.add(Method::Get, "/caf\xC3\xA9", answering("cafe"));

    EXPECT_FALSE(router.find(Method::Get, "/caf%C3%A9"));
    EXPECT_TRUE(router.find(Method::Get, "/caf\xC3\xA9"));
}

TEST(Router, HeadKeepsTheCapturesOfTheGetRouteItFallsBackTo) {
    Router router;
    router.add(Method::Get, "/users/<id>", answering("get"));

    const Match match = router.find(Method::Head, "/users/42");

    ASSERT_TRUE(match);
    EXPECT_EQ(answer_of(match), "get");
    EXPECT_EQ(match.params.get("id"), "42");
}

// Several patterns match this path, so the captures have to be the ones taken against the route that actually answered
// rather than whichever matched last.
TEST(Router, HeadKeepsTheCapturesOfTheRouteThatAnswers) {
    Router router;
    router.add(Method::Post, "/users/<posted>", answering("post"));
    router.add(Method::Get, "/users/<fetched>", answering("get"));
    router.add(Method::Delete, "/users/<removed>", answering("delete"));

    const Match match = router.find(Method::Head, "/users/42");

    ASSERT_TRUE(match);
    EXPECT_EQ(answer_of(match), "get");
    EXPECT_EQ(match.params.get("fetched"), "42");
    EXPECT_FALSE(match.params.get("posted").has_value());
    EXPECT_FALSE(match.params.get("removed").has_value());
}

// A 405 on a parameterised path owes the same Allow header a static one does, which is what one shared definition of
// matching buys.
TEST(Router, AllowsTheMethodsRegisteredForAPattern) {
    Router router;
    router.add(Method::Get, "/users/<id>", answering("get"));
    router.add(Method::Delete, "/users/<id>", answering("delete"));

    EXPECT_EQ(router.allowed_methods("/users/42"), (Methods{Method::Get, Method::Head, Method::Delete}));
    EXPECT_EQ(router.allowed_methods("/users/42/posts"), Methods{});
}

// Allow answers about a concrete path, not about a pattern, so two patterns that both serve it contribute to one list
// however they spell their names.
TEST(Router, AllowsAcrossEveryPatternThatServesThePath) {
    Router router;
    router.add(Method::Get, "/users/<id>", answering("get"));
    router.add(Method::Post, "/users/<name>", answering("post"));

    EXPECT_EQ(router.allowed_methods("/users/42"), (Methods{Method::Get, Method::Head, Method::Post}));
}

}  // namespace
