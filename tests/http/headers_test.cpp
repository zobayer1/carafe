#include <carafe/http/headers.hpp>

#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

namespace {

using carafe::http::HeaderField;
using carafe::http::Headers;

using Names = std::vector<std::string>;

// Collects field names in stored order.
Names names_of(const Headers& headers) {
    Names names;
    for (const auto& field : headers) {
        names.push_back(field.name);
    }
    return names;
}

// A fresh container holds nothing.
TEST(Headers, StartsEmpty) {
    const Headers headers;
    EXPECT_TRUE(headers.empty());
    EXPECT_EQ(headers.size(), 0U);
    EXPECT_FALSE(headers.get("host").has_value());
}

// add() is the boundary that enforces the lowercased-name invariant, so a caller
// building headers by hand cannot break lookup.
TEST(Headers, AddLowercasesTheName) {
    Headers headers;
    headers.add(HeaderField{"CONTENT-Type", "text/html"});
    EXPECT_EQ(names_of(headers), (Names{"content-type"}));
}

// The value is stored exactly as given; only the name is normalised.
TEST(Headers, AddPreservesValueCase) {
    Headers headers;
    headers.add(HeaderField{"etag", "AbC123"});
    EXPECT_EQ(headers.get("etag"), "AbC123");
}

// The lookup argument is folded on the fly, so callers may spell it any way.
TEST(Headers, GetIgnoresLookupCase) {
    Headers headers;
    headers.add(HeaderField{"content-length", "42"});
    EXPECT_EQ(headers.get("Content-Length"), "42");
    EXPECT_EQ(headers.get("CONTENT-LENGTH"), "42");
    EXPECT_EQ(headers.get("content-length"), "42");
}

// Repeated fields are legal, and get() answers with the first.
TEST(Headers, GetReturnsTheFirstOfRepeatedFields) {
    Headers headers;
    headers.add(HeaderField{"accept", "text/html"});
    headers.add(HeaderField{"accept", "application/json"});
    EXPECT_EQ(headers.get("accept"), "text/html");
}

// Absent and present-but-empty must not collapse: one is "no such field", the
// other is a field whose value is the empty string.
TEST(Headers, DistinguishesAbsentFromEmptyValue) {
    Headers headers;
    headers.add(HeaderField{"x-empty", ""});
    ASSERT_TRUE(headers.get("x-empty").has_value());
    EXPECT_TRUE(headers.get("x-empty")->empty());
    EXPECT_FALSE(headers.get("x-missing").has_value());
}

// contains() agrees with get(), including for the empty-value case.
TEST(Headers, ContainsMatchesGet) {
    Headers headers;
    headers.add(HeaderField{"x-empty", ""});
    EXPECT_TRUE(headers.contains("X-Empty"));
    EXPECT_FALSE(headers.contains("x-missing"));
}

// count() is what makes the singleton rules checkable: HTTP/1.1 needs exactly
// one Host, and conflicting Content-Length duplicates are a smuggling vector.
TEST(Headers, CountsRepeatedFields) {
    Headers headers;
    headers.add(HeaderField{"host", "a"});
    headers.add(HeaderField{"accept", "b"});
    headers.add(HeaderField{"accept", "c"});
    EXPECT_EQ(headers.count("x-missing"), 0U);
    EXPECT_EQ(headers.count("Host"), 1U);
    EXPECT_EQ(headers.count("ACCEPT"), 2U);
}

// Arrival order is preserved, which reserialisation and proxying depend on.
TEST(Headers, IterationPreservesInsertionOrder) {
    Headers headers;
    headers.add(HeaderField{"host", "a"});
    headers.add(HeaderField{"accept", "b"});
    headers.add(HeaderField{"host", "c"});
    EXPECT_EQ(names_of(headers), (Names{"host", "accept", "host"}));
    EXPECT_EQ(headers.size(), 3U);
    EXPECT_FALSE(headers.empty());
}

// A prefix of a stored name is not a match; matching compares whole names.
TEST(Headers, DoesNotMatchOnPrefix) {
    Headers headers;
    headers.add(HeaderField{"content-length", "42"});
    EXPECT_FALSE(headers.contains("content"));
    EXPECT_FALSE(headers.contains("content-length-extra"));
}

// Equal-length names must still be compared character by character; a length
// check alone would make every same-length name match.
TEST(Headers, DistinguishesNamesOfEqualLength) {
    Headers headers;
    headers.add(HeaderField{"accept", "a"});
    headers.add(HeaderField{"expect", "e"});
    EXPECT_EQ(headers.get("Accept"), "a");
    EXPECT_EQ(headers.get("Expect"), "e");
    EXPECT_FALSE(headers.contains("reject"));
}

// Non-ASCII bytes are compared, not folded: ASCII-only case folding is the whole
// reason std::tolower is avoided.
TEST(Headers, DoesNotFoldNonAsciiBytes) {
    Headers headers;
    headers.add(HeaderField{"x-\xC3\xA9", "v"});
    EXPECT_EQ(names_of(headers), (Names{"x-\xC3\xA9"}));
    EXPECT_TRUE(headers.contains("X-\xC3\xA9"));
}

}  // namespace
