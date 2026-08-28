#include <carafe/http/response.hpp>

#include <cstddef>
#include <string>
#include <string_view>

#include <gtest/gtest.h>

namespace {

using carafe::http::Response;
using carafe::http::status_message;

// How many times a field appears, which is the only way to catch a header that is written twice with different values.
std::size_t count_of(std::string_view text, std::string_view needle) {
    std::size_t found = 0;
    for (auto at = text.find(needle); at != std::string_view::npos; at = text.find(needle, at + needle.size())) {
        ++found;
    }
    return found;
}

std::string_view head_of(std::string_view response) {
    const auto blank = response.find("\r\n\r\n");
    return blank == std::string_view::npos ? response : response.substr(0, blank + 4);
}

std::string_view body_of(std::string_view response) {
    const auto blank = response.find("\r\n\r\n");
    return blank == std::string_view::npos ? std::string_view{} : response.substr(blank + 4);
}

TEST(Response, DefaultsToTwoHundredWithNothingToSay) {
    const std::string wire = Response{}.serialize();

    EXPECT_EQ(wire, "HTTP/1.1 200 OK\r\ncontent-length: 0\r\n\r\n");
}

TEST(Response, StatusLineCarriesTheCodeAndItsPhrase) {
    Response response;
    response.status = 400;

    EXPECT_EQ(head_of(response.serialize()).substr(0, 24), "HTTP/1.1 400 Bad Request");
}

// The space after the code belongs to the status line, not to the phrase, so a code with no phrase still has to produce
// it. Concatenating a leading space onto the phrase instead would malform exactly the lines nobody tests.
TEST(Response, UnknownStatusKeepsTheSpaceAndDropsThePhrase) {
    Response response;
    response.status = 418;

    EXPECT_TRUE(status_message(418).empty());
    EXPECT_EQ(head_of(response.serialize()).substr(0, 15), "HTTP/1.1 418 \r\n");
}

TEST(Response, ContentLengthIsTheBodySize) {
    Response response;
    response.body = "twelve bytes";

    const std::string wire = response.serialize();

    EXPECT_NE(wire.find("\r\ncontent-length: 12\r\n"), std::string::npos);
    EXPECT_EQ(body_of(wire), "twelve bytes");
}

// Counted in bytes rather than characters: a length in anything else leaves the client waiting for bytes that never
// arrive.
TEST(Response, ContentLengthCountsBytesNotCharacters) {
    Response response;
    response.body = "\xc3\xa9\xc3\xa9";  // two characters, four bytes

    EXPECT_NE(response.serialize().find("\r\ncontent-length: 4\r\n"), std::string::npos);
}

TEST(Response, HeadersComeOutInInsertionOrder) {
    Response response;
    response.headers.add({"x-first", "1"});
    response.headers.add({"x-second", "2"});

    const std::string wire = response.serialize();

    EXPECT_LT(wire.find("x-first: 1"), wire.find("x-second: 2"));
}

// A repeated field is legal and its order is meaningful, so serialize must not collapse the two into one.
TEST(Response, RepeatedFieldsAreBothWritten) {
    Response response;
    response.headers.add({"set-cookie", "a=1"});
    response.headers.add({"set-cookie", "b=2"});

    const std::string wire = response.serialize();

    EXPECT_NE(wire.find("set-cookie: a=1\r\n"), std::string::npos);
    EXPECT_NE(wire.find("set-cookie: b=2\r\n"), std::string::npos);
}

// Two content-lengths is worse than a wrong one: the client picks whichever it sees first, so the two ends disagree
// about where the body ends.
TEST(Response, CallerContentLengthIsReplacedRatherThanRepeated) {
    Response response;
    response.headers.add({"content-length", "999"});
    response.body = "hi";

    const std::string wire = response.serialize();

    EXPECT_EQ(count_of(wire, "content-length: "), 1U);
    EXPECT_NE(wire.find("\r\ncontent-length: 2\r\n"), std::string::npos);
    EXPECT_EQ(wire.find("999"), std::string::npos);
}

// Headers lowercases on the way in, and that is what reaches the wire: the property the serve tests' spellings depend
// on.
TEST(Response, FieldNamesReachTheWireLowercased) {
    Response response;
    response.headers.add({"Content-Type", "text/plain"});

    EXPECT_NE(response.serialize().find("\r\ncontent-type: text/plain\r\n"), std::string::npos);
}

// The point of the flag: same head, no body. A HEAD answer that recomputed its length from what it sent would claim
// zero, which is the classic bug.
TEST(Response, WithoutBodyWritesTheSameHeadAndStopsAtTheBlankLine) {
    Response response;
    response.headers.add({"content-type", "text/plain"});
    response.body = "twelve bytes";

    const std::string full = response.serialize(true);
    const std::string head = response.serialize(false);

    EXPECT_EQ(head, head_of(full));
    EXPECT_TRUE(body_of(head).empty());
    EXPECT_NE(head.find("\r\ncontent-length: 12\r\n"), std::string::npos);
}

TEST(Response, BodyIsWrittenExactlyAndNothingFollowsIt) {
    Response response;
    response.body = "line\r\n\r\nstill body";

    const std::string wire = response.serialize();

    // The body has a blank line of its own, so nothing may treat the last one as the boundary: the head ends at the
    // first.
    EXPECT_EQ(body_of(wire), "line\r\n\r\nstill body");
    EXPECT_EQ(wire.size(), head_of(wire).size() + response.body.size());
}

TEST(StatusMessage, NamesEveryStatusCarafeSends) {
    EXPECT_EQ(status_message(200), "OK");
    EXPECT_EQ(status_message(400), "Bad Request");
    EXPECT_EQ(status_message(404), "Not Found");
    EXPECT_EQ(status_message(405), "Method Not Allowed");
    EXPECT_EQ(status_message(414), "URI Too Long");
    EXPECT_EQ(status_message(431), "Request Header Fields Too Large");
    EXPECT_EQ(status_message(501), "Not Implemented");
    EXPECT_EQ(status_message(505), "HTTP Version Not Supported");
}

// Empty rather than a guess, so a handler may answer with a code carafe has never heard of and still produce a
// well-formed status line.
TEST(StatusMessage, IsEmptyForACodeItDoesNotKnow) {
    EXPECT_TRUE(status_message(0).empty());
    EXPECT_TRUE(status_message(418).empty());
    EXPECT_TRUE(status_message(599).empty());
}

}  // namespace
