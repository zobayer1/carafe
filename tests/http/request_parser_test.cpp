#include "http/request_parser.hpp"

#include "http/printers.hpp"

#include <string_view>

#include <gtest/gtest.h>

namespace {

using carafe::http::Method;
using carafe::http::parse_header_field;
using carafe::http::parse_request_line;
using carafe::http::ParseError;
using carafe::http::Version;

// Asserts `line` parses and that all three fields match.
void expect_request_line(std::string_view line, Method method, std::string_view target,
                         Version version) {
    SCOPED_TRACE(line);
    const auto result = parse_request_line(line);
    ASSERT_TRUE(result) << "unexpected error: " << result.error;
    EXPECT_EQ(result.value.method, method);
    EXPECT_EQ(result.value.target, target);
    EXPECT_EQ(result.value.version, version);
}

// Asserts `line` fails with exactly `expected`.
void expect_request_line_error(std::string_view line, ParseError expected) {
    SCOPED_TRACE(line);
    const auto result = parse_request_line(line);
    EXPECT_EQ(result.error, expected);
}

// Asserts `line` parses and that the field name and value match.
void expect_header_field(std::string_view line, std::string_view name, std::string_view value) {
    SCOPED_TRACE(line);
    const auto result = parse_header_field(line);
    ASSERT_TRUE(result.has_value()) << "failed to parse header field";
    EXPECT_EQ(result->name, name);
    EXPECT_EQ(result->value, value);
}

// Asserts `line` fails to parse as a header field.
void expect_header_field_rejected(std::string_view line) {
    SCOPED_TRACE(line);
    const auto result = parse_header_field(line);
    EXPECT_FALSE(result.has_value()) << "parsed as " << result->name << ": " << result->value;
}

// The shortest legal request line.
TEST(ParseRequestLine, MinimalGet) {
    expect_request_line("GET / HTTP/1.1", Method::Get, "/", Version::Http11);
}

// The query belongs to the target; this layer does not split it off.
TEST(ParseRequestLine, QueryStringStaysInTarget) {
    expect_request_line("GET /a/b?x=1&y=2 HTTP/1.1", Method::Get, "/a/b?x=1&y=2", Version::Http11);
}

// The other supported version.
TEST(ParseRequestLine, ParsesHttp10) {
    expect_request_line("POST /x HTTP/1.0", Method::Post, "/x", Version::Http10);
}

// Every entry in the lookup table, each with the target form it really uses.
TEST(ParseRequestLine, ParsesEveryMethod) {
    expect_request_line("GET /a HTTP/1.1", Method::Get, "/a", Version::Http11);
    expect_request_line("HEAD /a HTTP/1.1", Method::Head, "/a", Version::Http11);
    expect_request_line("POST /a HTTP/1.1", Method::Post, "/a", Version::Http11);
    expect_request_line("PUT /a HTTP/1.1", Method::Put, "/a", Version::Http11);
    expect_request_line("DELETE /a HTTP/1.1", Method::Delete, "/a", Version::Http11);
    expect_request_line("CONNECT example.com:443 HTTP/1.1", Method::Connect, "example.com:443",
                        Version::Http11);
    expect_request_line("OPTIONS * HTTP/1.1", Method::Options, "*", Version::Http11);
    expect_request_line("TRACE /a HTTP/1.1", Method::Trace, "/a", Version::Http11);
    expect_request_line("PATCH /a HTTP/1.1", Method::Patch, "/a", Version::Http11);
}

// Exactly three space-separated fields; fewer or more is malformed.
TEST(ParseRequestLine, RejectsWrongFieldCount) {
    expect_request_line_error("", ParseError::Malformed);
    expect_request_line_error("GET", ParseError::Malformed);
    expect_request_line_error("GET /", ParseError::Malformed);
    expect_request_line_error("GET / HTTP/1.1 extra", ParseError::Malformed);
    expect_request_line_error("GET / HTTP/1.1 ", ParseError::Malformed);
}

// An empty method is 400, not 501: there is no verb to be unimplemented.
TEST(ParseRequestLine, RejectsEmptyFields) {
    expect_request_line_error(" / HTTP/1.1", ParseError::Malformed);
    expect_request_line_error("GET / ", ParseError::Malformed);
    expect_request_line_error("GET  HTTP/1.1", ParseError::Malformed);
}

// Adjacent spaces make an empty field, not a skipped one.
TEST(ParseRequestLine, RejectsDoubleSpace) {
    expect_request_line_error("GET  /  HTTP/1.1", ParseError::Malformed);
}

// A bare CR survives a two-byte CRLF split, so it must not reach the target.
// The NUL case needs the sized constructor; the const char* one truncates.
TEST(ParseRequestLine, RejectsControlCharacters) {
    expect_request_line_error("GET /a\rHTTP/1.1 HTTP/1.1", ParseError::Malformed);
    expect_request_line_error("GET /a\r HTTP/1.1", ParseError::Malformed);
    expect_request_line_error("GET /a\nb HTTP/1.1", ParseError::Malformed);
    expect_request_line_error("GET /a\tb HTTP/1.1", ParseError::Malformed);
    expect_request_line_error("GET /a\x7F HTTP/1.1", ParseError::Malformed);
    expect_request_line_error(std::string_view("GET /a\0b HTTP/1.1", 17), ParseError::Malformed);
}

// Guards the unsigned char cast: as signed char, every byte above 0x7F reads
// as a control byte and any non-ASCII target 400s.
TEST(ParseRequestLine, AcceptsNonAsciiTarget) {
    expect_request_line("GET /caf\xC3\xA9 HTTP/1.1", Method::Get, "/caf\xC3\xA9", Version::Http11);
}

// Methods are case-sensitive tokens (RFC 9110 9.1), so "get" is not GET.
TEST(ParseRequestLine, RejectsUnknownMethod) {
    expect_request_line_error("FROB / HTTP/1.1", ParseError::UnknownMethod);
    expect_request_line_error("get / HTTP/1.1", ParseError::UnknownMethod);
    expect_request_line_error("GETT / HTTP/1.1", ParseError::UnknownMethod);
}

// Well-formed version, protocol we do not speak: 505.
TEST(ParseRequestLine, RejectsUnsupportedVersion) {
    expect_request_line_error("GET / HTTP/9.9", ParseError::UnsupportedVersion);
    expect_request_line_error("GET / HTTP/2.0", ParseError::UnsupportedVersion);
    expect_request_line_error("GET / HTTP/1.2", ParseError::UnsupportedVersion);
    expect_request_line_error("GET / HTTP/0.9", ParseError::UnsupportedVersion);
}

// Not a version string at all: 400. The boundary ParseError exists to draw.
TEST(ParseRequestLine, RejectsMalformedVersion) {
    expect_request_line_error("GET / FTP/1.1", ParseError::Malformed);
    expect_request_line_error("GET / HTTP/1", ParseError::Malformed);
    expect_request_line_error("GET / HTTP/1.", ParseError::Malformed);
    expect_request_line_error("GET / HTTP/11.1", ParseError::Malformed);
    expect_request_line_error("GET / HTTP/1.11", ParseError::Malformed);
    expect_request_line_error("GET / HTTP/x.1", ParseError::Malformed);
    expect_request_line_error("GET / HTTP/1.x", ParseError::Malformed);
    expect_request_line_error("GET / HTTP/1x1", ParseError::Malformed);
    expect_request_line_error("GET / http/1.1", ParseError::Malformed);
    expect_request_line_error("GET / XTTP/1.1",
                              ParseError::Malformed);  // reaches the prefix compare
}

// Both fields are bad; the version is checked first, so 505 wins.
TEST(ParseRequestLine, VersionErrorTakesPrecedenceOverMethodError) {
    expect_request_line_error("FROB / HTTP/9.9", ParseError::UnsupportedVersion);
}

// RFC 3986 2.1 admits no bare '%': each of these is a guess at what was meant.
// The last two reach the bounds test, one with the '%' at the front of the
// target and one with content ahead of it.
TEST(ParseRequestLine, RejectsMalformedPercentEscapes) {
    expect_request_line_error("GET /%zz HTTP/1.1", ParseError::Malformed);
    expect_request_line_error("GET /%2z HTTP/1.1", ParseError::Malformed);
    expect_request_line_error("GET /%z2 HTTP/1.1", ParseError::Malformed);
    expect_request_line_error("GET /%%41 HTTP/1.1", ParseError::Malformed);
    expect_request_line_error("GET /a?x=%2G HTTP/1.1", ParseError::Malformed);
    expect_request_line_error("GET /%4 HTTP/1.1", ParseError::Malformed);
    expect_request_line_error("GET /a%2 HTTP/1.1", ParseError::Malformed);
}

// Hex is case-insensitive, and the scan resumes past a good escape rather than
// re-reading its digits as the start of another. The query is checked too:
// pct-encoded is a URI-wide production, not a path one.
TEST(ParseRequestLine, AcceptsWellFormedPercentEscapes) {
    expect_request_line("GET /a%20b HTTP/1.1", Method::Get, "/a%20b", Version::Http11);
    expect_request_line("GET /%2F%2f HTTP/1.1", Method::Get, "/%2F%2f", Version::Http11);
    expect_request_line("GET /caf%C3%A9 HTTP/1.1", Method::Get, "/caf%C3%A9", Version::Http11);
    expect_request_line("GET /a?x=%26 HTTP/1.1", Method::Get, "/a?x=%26", Version::Http11);
}

// The target is tested after the version and before the method, so a bad one
// loses to a protocol we do not speak and beats a verb we do not implement.
TEST(ParseRequestLine, TargetErrorLosesToVersionAndBeatsMethod) {
    expect_request_line_error("GET /%zz HTTP/9.9", ParseError::UnsupportedVersion);
    expect_request_line_error("FROB /%zz HTTP/1.1", ParseError::Malformed);
}

// A known gap, not a decision: escapes are validated, but the target is still
// not checked against any of the four request-target forms RFC 9112 3.2 gives.
// Expected to fail the day origin-form and absolute-form are told apart.
TEST(ParseRequestLine, AcceptsAnyNonEmptyTargetForNow) {
    expect_request_line("GET foo HTTP/1.1", Method::Get, "foo", Version::Http11);
}

// Names are case-insensitive, so normalise once here instead of at every lookup.
TEST(ParseHeaderField, LowercasesTheName) {
    expect_header_field("Host:x", "host", "x");
    expect_header_field("CONTENT-LENGTH:0", "content-length", "0");
}

// tchar is more than letters: digits and a fixed punctuation set are names too.
TEST(ParseHeaderField, AcceptsEveryTokenCharacterClassInTheName) {
    expect_header_field("X-9:v", "x-9", "v");
    expect_header_field("!#$%&'*+-.^_`|~:v", "!#$%&'*+-.^_`|~", "v");
}

// Values are not case-insensitive: tokens, base64 and URLs all carry meaning in
// their casing.
TEST(ParseHeaderField, PreservesValueCase) {
    expect_header_field("Host:XA", "host", "XA");
}

// OWS around the value is optional, and it is SP or HTAB -- a trim written
// against ' ' alone passes the first of these and fails the second.
TEST(ParseHeaderField, TrimsOptionalWhitespaceAroundTheValue) {
    expect_header_field("Host:   xs", "host", "xs");
    expect_header_field("X-Tab:\t x \t", "x-tab", "x");
}

// Only the ends are trimmed; SP and HTAB inside the value are content.
TEST(ParseHeaderField, KeepsWhitespaceInsideTheValue) {
    expect_header_field("X-Tab:   a\tb", "x-tab", "a\tb");
}

// field-value is *field-content, so an empty value is legal syntax. An empty
// Host is invalid, but that is the assembler's rule, not this layer's.
TEST(ParseHeaderField, AcceptsEmptyValue) {
    expect_header_field("X-Empty:", "x-empty", "");
    expect_header_field("X-Empty:   ", "x-empty", "");
}

// A colon is ordinary content in a value, so only the first one splits.
TEST(ParseHeaderField, SplitsOnTheFirstColonOnly) {
    expect_header_field("Host:example.com:8080", "host", "example.com:8080");
}

// obs-text is opaque data, not an error. Also the case that catches a signed
// char comparison mistaking 0x80 for a control byte.
TEST(ParseHeaderField, AcceptsObsTextInValue) {
    expect_header_field("X-Obs:\x80\xFF", "x-obs", "\x80\xFF");
}

// Without a colon there is no field at all.
TEST(ParseHeaderField, RejectsMissingColon) {
    expect_header_field_rejected("Host");
}

// The name is a token, and a token cannot be empty.
TEST(ParseHeaderField, RejectsEmptyName) {
    expect_header_field_rejected(":x");
}

// RFC 9112 §5.1 requires rejecting rather than trimming: a proxy that trims and
// a server that does not would disagree about the field name.
TEST(ParseHeaderField, RejectsSpaceBeforeTheColon) {
    expect_header_field_rejected("Host : x");
}

// A line opening with OWS is an obs-fold continuation, deprecated by RFC 9112
// §5.2. Parsed as a field it would yield a nonsense name.
TEST(ParseHeaderField, RejectsObsFoldContinuation) {
    expect_header_field_rejected(" Host: x");
    expect_header_field_rejected("\tHost: x");
}

// Spaces and delimiters are not token characters.
TEST(ParseHeaderField, RejectsNonTokenCharactersInName) {
    expect_header_field_rejected("This is a host: x");
    expect_header_field_rejected("Host(x): y");
}

// CR, LF and DEL are invalid in a value. LineReader makes the first two
// unreachable, but this layer does not lean on that.
TEST(ParseHeaderField, RejectsControlCharactersInValue) {
    expect_header_field_rejected("Host: a\rb");
    expect_header_field_rejected("Host: a\nb");
    expect_header_field_rejected(
        "Host: a\x7F"
        "b");
}

// The legal set is HTAB, 0x20-0x7E and obs-text, so anything else below 0x20 is
// out. A denylist naming only CR, LF and DEL would let all of these through.
TEST(ParseHeaderField, RejectsOtherControlCharactersInValue) {
    expect_header_field_rejected(std::string_view("Host: a\0b", 9));
    expect_header_field_rejected(
        "Host: a\x01"
        "b");
    expect_header_field_rejected(
        "Host: a\x0B"
        "b");
    expect_header_field_rejected(
        "Host: a\x1F"
        "b");
}

// A NUL anywhere in the name is not a tchar either; the sized constructor is
// what gets it past strlen and into the parser at all.
TEST(ParseHeaderField, RejectsNulInName) {
    expect_header_field_rejected(std::string_view("Ho\0st: x", 8));
}

}  // namespace
