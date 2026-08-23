#include <carafe/http/request.hpp>

#include <gtest/gtest.h>

namespace {

using carafe::http::Method;
using carafe::http::method_name;

// The names go on the wire in an Allow header, where they are case-sensitive
// tokens rather than labels -- a lowercase one is a different method, and a
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

}  // namespace
