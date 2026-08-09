#include "http/request_parser.hpp"

#include <ostream>
#include <string_view>

#include <gtest/gtest.h>

// In carafe::http, not an anonymous namespace: gtest finds these by ADL.
namespace carafe::http {

std::ostream& operator<<(std::ostream& os, Method method) {
    switch (method) {
        case Method::Get:
            return os << "Get";
        case Method::Head:
            return os << "Head";
        case Method::Post:
            return os << "Post";
        case Method::Put:
            return os << "Put";
        case Method::Delete:
            return os << "Delete";
        case Method::Connect:
            return os << "Connect";
        case Method::Options:
            return os << "Options";
        case Method::Trace:
            return os << "Trace";
        case Method::Patch:
            return os << "Patch";
    }
    return os << "Method(" << static_cast<int>(method) << ")";
}

std::ostream& operator<<(std::ostream& os, Version version) {
    switch (version) {
        case Version::Http10:
            return os << "HTTP/1.0";
        case Version::Http11:
            return os << "HTTP/1.1";
    }
    return os << "Version(" << static_cast<int>(version) << ")";
}

std::ostream& operator<<(std::ostream& os, ParseError error) {
    switch (error) {
        case ParseError::None:
            return os << "None";
        case ParseError::Malformed:
            return os << "Malformed";
        case ParseError::UnknownMethod:
            return os << "UnknownMethod";
        case ParseError::UnsupportedVersion:
            return os << "UnsupportedVersion";
    }
    return os << "ParseError(" << static_cast<int>(error) << ")";
}

}  // namespace carafe::http

namespace {

using carafe::http::Method;
using carafe::http::parse_request_line;
using carafe::http::ParseError;
using carafe::http::Version;

// Asserts `line` parses and that all three fields match.
void expect_ok(std::string_view line, Method method, std::string_view target, Version version) {
    SCOPED_TRACE(line);
    const auto result = parse_request_line(line);
    ASSERT_TRUE(result) << "unexpected error: " << result.error;
    EXPECT_EQ(result.value.method, method);
    EXPECT_EQ(result.value.target, target);
    EXPECT_EQ(result.value.version, version);
}

// Asserts `line` fails with exactly `expected`.
void expect_error(std::string_view line, ParseError expected) {
    SCOPED_TRACE(line);
    const auto result = parse_request_line(line);
    EXPECT_EQ(result.error, expected);
}

// The shortest legal request line.
TEST(ParseRequestLine, MinimalGet) {
    expect_ok("GET / HTTP/1.1", Method::Get, "/", Version::Http11);
}

// The query belongs to the target; this layer does not split it off.
TEST(ParseRequestLine, QueryStringStaysInTarget) {
    expect_ok("GET /a/b?x=1&y=2 HTTP/1.1", Method::Get, "/a/b?x=1&y=2", Version::Http11);
}

// The other supported version.
TEST(ParseRequestLine, ParsesHttp10) {
    expect_ok("POST /x HTTP/1.0", Method::Post, "/x", Version::Http10);
}

// Every entry in the lookup table, each with the target form it really uses.
TEST(ParseRequestLine, ParsesEveryMethod) {
    expect_ok("GET /a HTTP/1.1", Method::Get, "/a", Version::Http11);
    expect_ok("HEAD /a HTTP/1.1", Method::Head, "/a", Version::Http11);
    expect_ok("POST /a HTTP/1.1", Method::Post, "/a", Version::Http11);
    expect_ok("PUT /a HTTP/1.1", Method::Put, "/a", Version::Http11);
    expect_ok("DELETE /a HTTP/1.1", Method::Delete, "/a", Version::Http11);
    expect_ok("CONNECT example.com:443 HTTP/1.1", Method::Connect, "example.com:443",
              Version::Http11);
    expect_ok("OPTIONS * HTTP/1.1", Method::Options, "*", Version::Http11);
    expect_ok("TRACE /a HTTP/1.1", Method::Trace, "/a", Version::Http11);
    expect_ok("PATCH /a HTTP/1.1", Method::Patch, "/a", Version::Http11);
}

// Exactly three space-separated fields; fewer or more is malformed.
TEST(ParseRequestLine, RejectsWrongFieldCount) {
    expect_error("", ParseError::Malformed);
    expect_error("GET", ParseError::Malformed);
    expect_error("GET /", ParseError::Malformed);
    expect_error("GET / HTTP/1.1 extra", ParseError::Malformed);
    expect_error("GET / HTTP/1.1 ", ParseError::Malformed);
}

// An empty method is 400, not 501: there is no verb to be unimplemented.
TEST(ParseRequestLine, RejectsEmptyFields) {
    expect_error(" / HTTP/1.1", ParseError::Malformed);
    expect_error("GET / ", ParseError::Malformed);
    expect_error("GET  HTTP/1.1", ParseError::Malformed);
}

// Adjacent spaces make an empty field, not a skipped one.
TEST(ParseRequestLine, RejectsDoubleSpace) {
    expect_error("GET  /  HTTP/1.1", ParseError::Malformed);
}

// A bare CR survives a two-byte CRLF split, so it must not reach the target.
// The NUL case needs the sized constructor; the const char* one truncates.
TEST(ParseRequestLine, RejectsControlCharacters) {
    expect_error("GET /a\rHTTP/1.1 HTTP/1.1", ParseError::Malformed);
    expect_error("GET /a\r HTTP/1.1", ParseError::Malformed);
    expect_error("GET /a\nb HTTP/1.1", ParseError::Malformed);
    expect_error("GET /a\tb HTTP/1.1", ParseError::Malformed);
    expect_error("GET /a\x7F HTTP/1.1", ParseError::Malformed);
    expect_error(std::string_view("GET /a\0b HTTP/1.1", 17), ParseError::Malformed);
}

// Guards the unsigned char cast: as signed char, every byte above 0x7F reads
// as a control byte and any non-ASCII target 400s.
TEST(ParseRequestLine, AcceptsNonAsciiTarget) {
    expect_ok("GET /caf\xC3\xA9 HTTP/1.1", Method::Get, "/caf\xC3\xA9", Version::Http11);
}

// Methods are case-sensitive tokens (RFC 9110 9.1), so "get" is not GET.
TEST(ParseRequestLine, RejectsUnknownMethod) {
    expect_error("FROB / HTTP/1.1", ParseError::UnknownMethod);
    expect_error("get / HTTP/1.1", ParseError::UnknownMethod);
    expect_error("GETT / HTTP/1.1", ParseError::UnknownMethod);
}

// Well-formed version, protocol we do not speak: 505.
TEST(ParseRequestLine, RejectsUnsupportedVersion) {
    expect_error("GET / HTTP/9.9", ParseError::UnsupportedVersion);
    expect_error("GET / HTTP/2.0", ParseError::UnsupportedVersion);
    expect_error("GET / HTTP/1.2", ParseError::UnsupportedVersion);
    expect_error("GET / HTTP/0.9", ParseError::UnsupportedVersion);
}

// Not a version string at all: 400. The boundary ParseError exists to draw.
TEST(ParseRequestLine, RejectsMalformedVersion) {
    expect_error("GET / FTP/1.1", ParseError::Malformed);
    expect_error("GET / HTTP/1", ParseError::Malformed);
    expect_error("GET / HTTP/1.", ParseError::Malformed);
    expect_error("GET / HTTP/11.1", ParseError::Malformed);
    expect_error("GET / HTTP/1.11", ParseError::Malformed);
    expect_error("GET / HTTP/x.1", ParseError::Malformed);
    expect_error("GET / HTTP/1.x", ParseError::Malformed);
    expect_error("GET / HTTP/1x1", ParseError::Malformed);
    expect_error("GET / http/1.1", ParseError::Malformed);
    expect_error("GET / XTTP/1.1", ParseError::Malformed);  // reaches the prefix compare
}

// Both fields are bad; the version is checked first, so 505 wins.
TEST(ParseRequestLine, VersionErrorTakesPrecedenceOverMethodError) {
    expect_error("FROB / HTTP/9.9", ParseError::UnsupportedVersion);
}

// A known gap, not a decision: the target is only checked for being non-empty
// and space-free. Expected to fail the day request-target validation lands.
TEST(ParseRequestLine, AcceptsAnyNonEmptyTargetForNow) {
    expect_ok("GET foo HTTP/1.1", Method::Get, "foo", Version::Http11);
    expect_ok("GET %zz HTTP/1.1", Method::Get, "%zz", Version::Http11);
}

}  // namespace
