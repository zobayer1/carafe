#include <carafe/http/request.hpp>

#include <gtest/gtest.h>

namespace {

using carafe::http::Method;
using carafe::http::method_name;
using carafe::http::Params;

// The names go on the wire in an Allow header, where they are case-sensitive
// tokens rather than labels: a lowercase one is a different method, and a
// misspelled one is no method at all.
TEST(MethodName, SpellsEveryMethodCarafeParses) {
    EXPECT_EQ(method_name(Method::Get), "GET");
    EXPECT_EQ(method_name(Method::Head), "HEAD");
    EXPECT_EQ(method_name(Method::Post), "POST");
    EXPECT_EQ(method_name(Method::Put), "PUT");
    EXPECT_EQ(method_name(Method::Delete), "DELETE");
    EXPECT_EQ(method_name(Method::Connect), "CONNECT");
    EXPECT_EQ(method_name(Method::Options), "OPTIONS");
    EXPECT_EQ(method_name(Method::Trace), "TRACE");
    EXPECT_EQ(method_name(Method::Patch), "PATCH");
}

// Only a bug reaches this, since every Method comes from the parser. It answers
// empty rather than reading off the end of a table, which keeps the failure to
// one missing name instead of a malformed header.
TEST(MethodName, IsEmptyForAValueOutsideTheEnum) {
    // Well defined for a scoped enum, whose value range is its underlying type's;
    // the check is conservative, and reaching the fallback is the point here.
    // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)
    EXPECT_TRUE(method_name(static_cast<Method>(99)).empty());
}

// The name comes from the pattern the route was registered with, so a handler
// asks for what it wrote rather than for whatever the client happened to send.
TEST(Params, FindsTheValueBoundToAName) {
    const Params params{{{"id", "42"}, {"slug", "hello"}}};

    EXPECT_EQ(params.get("id"), "42");
    EXPECT_EQ(params.get("slug"), "hello");
}

TEST(Params, IsEmptyForANameNothingBound) {
    const Params params{{{"id", "42"}}};

    EXPECT_FALSE(params.get("slug").has_value());
}

TEST(Params, IsEmptyWhenNothingWasCaptured) {
    const Params params;

    EXPECT_FALSE(params.get("id").has_value());
}

// A pattern binding one name twice is odd rather than wrong, and add() has no
// channel to refuse it on. The first reads as the one that was meant.
TEST(Params, AnswersTheFirstOfSeveralEntriesWithOneName) {
    const Params params{{{"id", "first"}, {"id", "second"}}};

    EXPECT_EQ(params.get("id"), "first");
}

// Why get() answers an optional rather than an empty string: the router will
// not capture an empty segment, but Params is an open struct that cannot
// promise that, so empty has to stay a value and not a way of saying "absent".
TEST(Params, TellsAnEmptyValueApartFromNoValue) {
    const Params params{{{"id", ""}}};

    ASSERT_TRUE(params.get("id").has_value());
    EXPECT_TRUE(params.get("id")->empty());
    EXPECT_FALSE(params.get("missing").has_value());
}

// Unlike a header name, which arrives in whatever case the client chose, a
// parameter name is written twice by the same person, in the pattern and in the
// handler. Folding case would only hide a typo.
TEST(Params, MatchesNamesCaseSensitively) {
    const Params params{{{"id", "42"}}};

    EXPECT_FALSE(params.get("ID").has_value());
    EXPECT_FALSE(params.get("Id").has_value());
}

}  // namespace
