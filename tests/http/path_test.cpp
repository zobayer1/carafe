#include "http/path.hpp"

#include <string>

#include <gtest/gtest.h>

namespace {

using carafe::http::normalize_path;

// A path that is already in its normal form must come back untouched, or normalisation would be a second spelling
// rather than one canonical answer.
TEST(NormalizePath, LeavesAPathThatIsAlreadyNormalAlone) {
    EXPECT_EQ(normalize_path("/"), "/");
    EXPECT_EQ(normalize_path("/a"), "/a");
    EXPECT_EQ(normalize_path("/a/b"), "/a/b");
    EXPECT_EQ(normalize_path("/a/b/"), "/a/b/");
}

// "." names the directory it sits in, so it contributes nothing to a path. RFC 3986 §5.2.4 still leaves the slash it
// stood after, which is why the last two lines keep a trailing one.
TEST(NormalizePath, DropsSegmentsThatNameWhereTheyAlreadyAre) {
    EXPECT_EQ(normalize_path("/a/./b"), "/a/b");
    EXPECT_EQ(normalize_path("/./a"), "/a");
    EXPECT_EQ(normalize_path("/a/."), "/a/");
    EXPECT_EQ(normalize_path("/a//."), "/a//");
}

// ".." names the segment before it, so the pair cancels. A trailing one leaves the slash behind, per §5.2.4: "/a/b/.."
// is the directory "/a/", not the resource "/a".
TEST(NormalizePath, PopsTheSegmentBeforeADoubleDot) {
    EXPECT_EQ(normalize_path("/a/b/../c"), "/a/c");
    EXPECT_EQ(normalize_path("/a/b/.."), "/a/");
    EXPECT_EQ(normalize_path("/a/../b"), "/b");
}

// The reason this function exists. A target may ask for as many parents as it likes and still name something inside
// the tree, because ".." at the root pops nothing rather than stepping outside it.
TEST(NormalizePath, CannotClimbAboveTheRoot) {
    EXPECT_EQ(normalize_path("/../a"), "/a");
    EXPECT_EQ(normalize_path("/a/../../b"), "/b");
    EXPECT_EQ(normalize_path("/../../.."), "/");
    EXPECT_EQ(normalize_path("/.."), "/");
    EXPECT_EQ(normalize_path("/a/b/../.."), "/");
}

// Escapes are resolved before segments are split, so a dot segment spelled in hex is still a dot segment. Splitting
// first would leave "%2e%2e" looking like an ordinary name and let it through as a capture.
TEST(NormalizePath, ResolvesADotSegmentThatArrivedEncoded) {
    EXPECT_EQ(normalize_path("/a/%2e%2e/b"), "/b");
    EXPECT_EQ(normalize_path("/a/%2E%2E/b"), "/b");
    EXPECT_EQ(normalize_path("/a/%2e/b"), "/a/b");
}

// The other half of that order: a separator stays escaped however it was spelled, so no client can introduce a segment
// boundary the split will not see. "%2f" is one character of a name, not the end of one.
TEST(NormalizePath, LeavesAnEscapedSeparatorEscaped) {
    EXPECT_EQ(normalize_path("/a%2fb"), "/a%2Fb");
    EXPECT_EQ(normalize_path("/a%2Fb"), "/a%2Fb");
    EXPECT_EQ(normalize_path("/a/%2f../b"), "/a/%2F../b");
}

// RFC 3986 §2.3: escaping an unreserved character changes nothing, so resolving its escape changes nothing either.
// Everything else keeps its escape, because decoding it could change which resource the path names.
TEST(NormalizePath, ResolvesOnlyTheEscapesThatMeanNothing) {
    EXPECT_EQ(normalize_path("/%41%42"), "/AB");
    EXPECT_EQ(normalize_path("/%61-%5F.%7E"), "/a-_.~");
    EXPECT_EQ(normalize_path("/%30%39"), "/09");
    EXPECT_EQ(normalize_path("/a%3Fb"), "/a%3Fb");
    EXPECT_EQ(normalize_path("/a%20b"), "/a%20b");
}

// RFC 3986 §6.2.2.1: an escape it keeps is uppercased, so two spellings of one byte compare equal instead of routing
// to two different places.
TEST(NormalizePath, UppercasesTheHexOfAnEscapeItKeeps) {
    EXPECT_EQ(normalize_path("/a%3fb"), "/a%3Fb");
    EXPECT_EQ(normalize_path("/a%2bb"), "/a%2Bb");
}

// An empty segment is kept rather than merged away. The RFC does not collapse them, and a pattern cannot match one, so
// "//a" is a clean miss instead of a path that quietly means two things.
TEST(NormalizePath, KeepsEmptySegments) {
    EXPECT_EQ(normalize_path("//a"), "//a");
    EXPECT_EQ(normalize_path("/a//b"), "/a//b");
    EXPECT_EQ(normalize_path("//"), "//");
}

// Not every request-target begins with a slash: "OPTIONS *" is the asterisk-form of RFC 9112 §3.2.4. Nothing here may
// invent a leading slash for it, or it would start matching routes.
TEST(NormalizePath, DoesNotInventALeadingSlash) {
    EXPECT_EQ(normalize_path("*"), "*");
    EXPECT_EQ(normalize_path("a/b"), "a/b");
    EXPECT_EQ(normalize_path(""), "");
}

// A relative path has no root to stop at, so ".." there pops until nothing is left rather than clamping.
TEST(NormalizePath, PopsToNothingWithoutARootToStopAt) {
    EXPECT_EQ(normalize_path("a/.."), "");
    EXPECT_EQ(normalize_path("a/../../b"), "b");
}

// The parser rejects a malformed escape before routing sees it, so this is unreachable in a served request. It is
// pinned anyway because the guard that makes it unreachable here is a bounds test: a truncated escape at the very end
// reads past the string the moment that test stops being believed.
TEST(NormalizePath, CopiesAMalformedEscapeAsItStands) {
    EXPECT_EQ(normalize_path("/a%4"), "/a%4");
    EXPECT_EQ(normalize_path("/a%"), "/a%");
    EXPECT_EQ(normalize_path("%"), "%");
    EXPECT_EQ(normalize_path("/x/%zz/y"), "/x/%zz/y");
}

}  // namespace
