#include "http/request_reader.hpp"

#include "http/printers.hpp"

#include <cstddef>
#include <string>
#include <string_view>

#include <gtest/gtest.h>

namespace {

using carafe::http::Method;
using carafe::http::RequestError;
using carafe::http::RequestReader;
using carafe::http::RequestResult;
using carafe::http::Version;

// Mirror the caps in request_reader.cpp and line_reader.cpp, which tests cannot
// reach. Crossing each from both sides keeps drift from being silent.
constexpr std::size_t max_head_bytes = 65536;
constexpr std::size_t max_header_fields = 100;
constexpr std::size_t max_line_length = 8192;
constexpr std::size_t max_body_bytes = 1048576;

// One append, one answer.
RequestResult read(RequestReader& reader, std::string_view bytes) {
    reader.append(bytes);
    return reader.next_request();
}

// Feeds one byte at a time, stopping at the first completed request or failure,
// so every chunk boundary in the head is exercised.
RequestResult read_bytewise(RequestReader& reader, std::string_view bytes) {
    RequestResult result;
    for (const char ch : bytes) {
        reader.append(std::string_view(&ch, 1));
        result = reader.next_request();
        if (!result || result.request) {
            return result;
        }
    }
    return result;
}

// A POST declaring the length of `body` and carrying it.
std::string post_body(std::string_view body) {
    return "POST /p HTTP/1.1\r\nHost: x\r\nContent-Length: " + std::to_string(body.size()) +
           "\r\n\r\n" + std::string{body};
}

// A POST whose Content-Length carries `value`, with no body following.
RequestError content_length_error(std::string_view value) {
    RequestReader reader;
    std::string bytes = "POST /p HTTP/1.1\r\nHost: x\r\nContent-Length: ";
    bytes += value;
    bytes += "\r\n\r\n";
    return read(reader, bytes).error;
}

// `count` field lines, each value `value_size` bytes, with distinct names.
std::string pad_fields(std::size_t count, std::size_t value_size) {
    std::string out;
    for (std::size_t i = 0; i < count; ++i) {
        out += "x-pad-" + std::to_string(i) + ": " + std::string(value_size, 'v') + "\r\n";
    }
    return out;
}

// A padding field of exactly `value_size + pad_field_overhead` wire bytes. The
// index is zero-padded so the size does not depend on how many came before it.
constexpr std::size_t pad_field_overhead = 13;  // "x-pad-NNN: " + CRLF

std::string pad_field(std::size_t index, std::size_t value_size) {
    std::string digits = std::to_string(index);
    digits.insert(digits.begin(), 3 - digits.size(), '0');
    return "x-pad-" + digits + ": " + std::string(value_size, 'v') + "\r\n";
}

// A complete head whose wire size, every terminator included, is exactly
// `bytes`. Padding is split across several fields because no single line may
// exceed max_line_length.
std::string head_of_size(std::size_t bytes) {
    const std::string prefix = "GET / HTTP/1.1\r\nHost: x\r\n";
    constexpr std::size_t chunk = 4000;

    std::size_t budget = bytes - prefix.size() - 2;  // 2 for the blank line
    std::string pads;
    std::size_t index = 0;
    while (budget >= chunk + (2 * pad_field_overhead)) {
        pads += pad_field(index++, chunk);
        budget -= chunk + pad_field_overhead;
    }
    pads += pad_field(index, budget - pad_field_overhead);

    return prefix + pads + "\r\n";
}

// The shortest request this server accepts: HTTP/1.1 demands a Host.
TEST(RequestReader, ParsesMinimalRequest) {
    RequestReader reader;
    const auto result = read(reader, "GET / HTTP/1.1\r\nHost: example.com\r\n\r\n");
    ASSERT_TRUE(result) << "unexpected error: " << result.error;
    ASSERT_TRUE(result.request.has_value());
    EXPECT_EQ(result.request->method, Method::Get);
    EXPECT_EQ(result.request->target, "/");
    EXPECT_EQ(result.request->version, Version::Http11);
    EXPECT_EQ(result.request->headers.get("host"), "example.com");
}

// Every field reaches the container, normalised and in order.
TEST(RequestReader, ParsesEveryField) {
    RequestReader reader;
    const auto result = read(reader,
                             "POST /submit HTTP/1.1\r\n"
                             "Host: example.com\r\n"
                             "Content-Length: 4\r\n"
                             "Accept: text/html\r\n"
                             "Accept: application/json\r\n"
                             "\r\n"
                             "abcd");
    ASSERT_TRUE(result) << "unexpected error: " << result.error;
    ASSERT_TRUE(result.request.has_value());
    EXPECT_EQ(result.request->method, Method::Post);
    EXPECT_EQ(result.request->headers.size(), 4U);
    EXPECT_EQ(result.request->headers.get("CONTENT-LENGTH"), "4");
    EXPECT_EQ(result.request->headers.count("accept"), 2U);
    EXPECT_EQ(result.request->headers.get("accept"), "text/html");
}

// Host became mandatory in 1.1; a 1.0 request without one is still well formed.
TEST(RequestReader, AcceptsHttp10WithoutHost) {
    RequestReader reader;
    const auto result = read(reader, "GET / HTTP/1.0\r\n\r\n");
    ASSERT_TRUE(result) << "unexpected error: " << result.error;
    ASSERT_TRUE(result.request.has_value());
    EXPECT_EQ(result.request->version, Version::Http10);
    EXPECT_TRUE(result.request->headers.empty());
}

// The whole head delivered one byte per append, which is the shape hostile
// input takes and the case every resumed scan has to survive.
TEST(RequestReader, SurvivesBytewiseDelivery) {
    RequestReader reader;
    const auto result =
        read_bytewise(reader, "GET /a?b=c HTTP/1.1\r\nHost: x\r\nAccept: */*\r\n\r\n");
    ASSERT_TRUE(result) << "unexpected error: " << result.error;
    ASSERT_TRUE(result.request.has_value());
    EXPECT_EQ(result.request->target, "/a?b=c");
    EXPECT_EQ(result.request->headers.get("accept"), "*/*");
}

// The blank line ends the head. Until it arrives there is no request, and that
// must not read as a failure.
TEST(RequestReader, WaitsForTheBlankLine) {
    RequestReader reader;
    const auto pending = read(reader, "GET / HTTP/1.1\r\nHost: x\r\n");
    EXPECT_TRUE(pending) << "unexpected error: " << pending.error;
    EXPECT_FALSE(pending.request.has_value());

    const auto done = read(reader, "\r\n");
    ASSERT_TRUE(done) << "unexpected error: " << done.error;
    ASSERT_TRUE(done.request.has_value());
    EXPECT_EQ(done.request->target, "/");
}

// Completion resets rather than terminating, so a pipelined connection yields
// its requests one call at a time from bytes that arrived together.
TEST(RequestReader, YieldsPipelinedRequestsInTurn) {
    RequestReader reader;
    reader.append(
        "GET /a HTTP/1.1\r\nHost: x\r\n\r\n"
        "GET /b HTTP/1.1\r\nHost: y\r\n\r\n");

    const auto first = reader.next_request();
    ASSERT_TRUE(first.request.has_value());
    EXPECT_EQ(first.request->target, "/a");
    EXPECT_EQ(first.request->headers.get("host"), "x");

    const auto second = reader.next_request();
    ASSERT_TRUE(second.request.has_value());
    EXPECT_EQ(second.request->target, "/b");
    EXPECT_EQ(second.request->headers.get("host"), "y");
}

// State from the first request must not leak into the second.
TEST(RequestReader, ResetsFieldsBetweenRequests) {
    RequestReader reader;
    reader.append(
        "GET /a HTTP/1.1\r\nHost: x\r\nAccept: text/html\r\n\r\n"
        "GET /b HTTP/1.0\r\n\r\n");

    const auto first = reader.next_request();
    ASSERT_TRUE(first.request.has_value());
    EXPECT_EQ(first.request->headers.size(), 2U);

    const auto second = reader.next_request();
    ASSERT_TRUE(second.request.has_value());
    EXPECT_TRUE(second.request->headers.empty());
    EXPECT_EQ(second.request->version, Version::Http10);
}

// The per-request budgets reset with the phase. Without that, a long-lived
// connection starts rejecting requests that are individually well within both
// caps -- and no branch is missing, so only feeding two requests finds it.
TEST(RequestReader, ResetsBudgetsBetweenRequests) {
    const std::string head = "GET / HTTP/1.1\r\nHost: x\r\n" + pad_fields(60, 600) + "\r\n";
    ASSERT_LT(head.size(), max_head_bytes);
    ASSERT_GT(head.size() * 2, max_head_bytes);

    RequestReader reader;
    reader.append(head + head);

    const auto first = reader.next_request();
    ASSERT_TRUE(first.request.has_value()) << "unexpected error: " << first.error;
    const auto second = reader.next_request();
    ASSERT_TRUE(second.request.has_value()) << "unexpected error: " << second.error;
}

// The request line's own failures reach the caller unchanged.
TEST(RequestReader, ReportsRequestLineFailures) {
    RequestReader reader1;
    EXPECT_EQ(read(reader1, "GET /\r\n\r\n").error, RequestError::Malformed);

    RequestReader reader2;
    EXPECT_EQ(read(reader2, "FROB / HTTP/1.1\r\n\r\n").error, RequestError::UnknownMethod);

    RequestReader reader3;
    EXPECT_EQ(read(reader3, "GET / HTTP/2.0\r\n\r\n").error, RequestError::UnsupportedVersion);
}

// A field that will not parse is 400, the only failure that layer has.
TEST(RequestReader, RejectsMalformedField) {
    RequestReader reader;
    const auto result = read(reader, "GET / HTTP/1.1\r\nHost : example.com\r\n\r\n");
    EXPECT_EQ(result.error, RequestError::Malformed);
}

// One LineError::LineTooLong, two outcomes: which one depends on the phase, and
// resolving that ambiguity is why this class exists.
TEST(RequestReader, DistinguishesOverlongRequestLineFromOverlongField) {
    RequestReader reader1;
    const std::string long_target(max_line_length, 'a');
    EXPECT_EQ(read(reader1, "GET /" + long_target + " HTTP/1.1\r\n").error,
              RequestError::RequestLineTooLong);

    RequestReader reader2;
    const std::string long_value(max_line_length, 'v');
    EXPECT_EQ(read(reader2, "GET / HTTP/1.1\r\nX-Long: " + long_value + "\r\n").error,
              RequestError::HeaderTooLong);
}

// A field count cap the byte cap cannot supply: these fields are tiny, so only
// counting them catches this.
TEST(RequestReader, RejectsTooManyFields) {
    RequestReader reader;
    const auto result =
        read(reader, "GET / HTTP/1.1\r\nHost: x\r\n" + pad_fields(max_header_fields, 1) + "\r\n");
    EXPECT_EQ(result.error, RequestError::TooManyHeaders);
}

// Exactly at the cap is legal; Host is one of the counted fields.
TEST(RequestReader, AcceptsFieldsUpToTheCap) {
    RequestReader reader;
    const auto result = read(
        reader, "GET / HTTP/1.1\r\nHost: x\r\n" + pad_fields(max_header_fields - 1, 1) + "\r\n");
    ASSERT_TRUE(result) << "unexpected error: " << result.error;
    ASSERT_TRUE(result.request.has_value());
    EXPECT_EQ(result.request->headers.size(), max_header_fields);
}

// The cap is a maximum, not a threshold. Few enough fields that the count cap
// cannot be what accepts or rejects these two.
TEST(RequestReader, AcceptsHeadAtExactlyTheCap) {
    const std::string head = head_of_size(max_head_bytes);
    ASSERT_EQ(head.size(), max_head_bytes);

    RequestReader reader;
    const auto result = read(reader, head);
    ASSERT_TRUE(result) << "unexpected error: " << result.error;
    EXPECT_TRUE(result.request.has_value());
}

// One byte more, and it is 431. Being exact is what pins the accounting: an
// implementation that forgot to count the terminators lands under the cap here
// and accepts.
TEST(RequestReader, RejectsHeadOneByteOverTheCap) {
    const std::string head = head_of_size(max_head_bytes + 1);
    ASSERT_EQ(head.size(), max_head_bytes + 1);

    RequestReader reader;
    EXPECT_EQ(read(reader, head).error, RequestError::HeadTooLarge);
}

// RFC 9112 3.2: an HTTP/1.1 request needs exactly one Host. None is 400.
TEST(RequestReader, RejectsHttp11WithoutHost) {
    RequestReader reader;
    EXPECT_EQ(read(reader, "GET / HTTP/1.1\r\n\r\n").error, RequestError::Malformed);
}

// Two Hosts are 400 as well: a proxy and this server could pick different ones.
TEST(RequestReader, RejectsDuplicateHost) {
    RequestReader reader;
    const auto result = read(reader, "GET / HTTP/1.1\r\nHost: a\r\nHost: b\r\n\r\n");
    EXPECT_EQ(result.error, RequestError::Malformed);
}

// There is no resynchronising a broken head: the caller must close, and feeding
// more bytes must not produce a request.
TEST(RequestReader, FailureIsTerminal) {
    RequestReader reader;
    EXPECT_EQ(read(reader, "BOGUS\r\n").error, RequestError::Malformed);

    const auto after = read(reader, "GET / HTTP/1.1\r\nHost: x\r\n\r\n");
    EXPECT_EQ(after.error, RequestError::Malformed);
    EXPECT_FALSE(after.request.has_value());
}

TEST(RequestReader, ReadsABodyOfTheDeclaredLength) {
    RequestReader reader;
    const auto result = read(reader, post_body("hello"));
    ASSERT_TRUE(result) << "unexpected error: " << result.error;
    ASSERT_TRUE(result.request.has_value());
    EXPECT_EQ(result.request->body, "hello");
}

// Until the body is whole there is no request, and that must not read as a failure.
TEST(RequestReader, WaitsForTheWholeBody) {
    RequestReader reader;
    const auto pending =
        read(reader, "POST /p HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\n\r\nhel");
    EXPECT_TRUE(pending) << "unexpected error: " << pending.error;
    EXPECT_FALSE(pending.request.has_value());

    const auto done = read(reader, "lo");
    ASSERT_TRUE(done) << "unexpected error: " << done.error;
    ASSERT_TRUE(done.request.has_value());
    EXPECT_EQ(done.request->body, "hello");
}

TEST(RequestReader, SurvivesBytewiseBodyDelivery) {
    RequestReader reader;
    const auto result = read_bytewise(reader, post_body("abcd"));
    ASSERT_TRUE(result) << "unexpected error: " << result.error;
    ASSERT_TRUE(result.request.has_value());
    EXPECT_EQ(result.request->body, "abcd");
}

// No Content-Length is no body, rather than a body running to end of stream.
TEST(RequestReader, TreatsAMissingContentLengthAsNoBody) {
    RequestReader reader;
    const auto result = read(reader, "POST /p HTTP/1.1\r\nHost: x\r\n\r\n");
    ASSERT_TRUE(result) << "unexpected error: " << result.error;
    ASSERT_TRUE(result.request.has_value());
    EXPECT_TRUE(result.request->body.empty());
}

TEST(RequestReader, TreatsAZeroContentLengthAsNoBody) {
    RequestReader reader;
    const auto result = read(reader, "POST /p HTTP/1.1\r\nHost: x\r\nContent-Length: 0\r\n\r\n");
    ASSERT_TRUE(result) << "unexpected error: " << result.error;
    ASSERT_TRUE(result.request.has_value());
    EXPECT_TRUE(result.request->body.empty());
}

// The bug this closes: unconsumed body bytes used to parse as the next request
// line, answering an ordinary GET with 501.
TEST(RequestReader, KeepsBodyBytesOutOfTheNextRequestLine) {
    RequestReader reader;
    reader.append(
        "POST /a HTTP/1.1\r\nHost: x\r\nContent-Length: 3\r\n\r\nabc"
        "GET /b HTTP/1.1\r\nHost: y\r\n\r\n");

    const auto first = reader.next_request();
    ASSERT_TRUE(first.request.has_value()) << "unexpected error: " << first.error;
    EXPECT_EQ(first.request->target, "/a");
    EXPECT_EQ(first.request->body, "abc");

    const auto second = reader.next_request();
    ASSERT_TRUE(second.request.has_value()) << "unexpected error: " << second.error;
    EXPECT_EQ(second.request->method, Method::Get);
    EXPECT_EQ(second.request->target, "/b");
    EXPECT_TRUE(second.request->body.empty());
}

// Framing does not consult the method: five bytes are five bytes whatever the
// verb means by them.
TEST(RequestReader, ReadsABodyOnAGet) {
    RequestReader reader;
    const auto result = read(reader, "GET /p HTTP/1.1\r\nHost: x\r\nContent-Length: 2\r\n\r\nhi");
    ASSERT_TRUE(result) << "unexpected error: " << result.error;
    ASSERT_TRUE(result.request.has_value());
    EXPECT_EQ(result.request->body, "hi");
}

TEST(RequestReader, ResetsTheBodyBetweenRequests) {
    RequestReader reader;
    reader.append(post_body("abc") + "GET /b HTTP/1.1\r\nHost: y\r\n\r\n");

    ASSERT_TRUE(reader.next_request().request.has_value());
    const auto second = reader.next_request();
    ASSERT_TRUE(second.request.has_value()) << "unexpected error: " << second.error;
    EXPECT_TRUE(second.request->body.empty());
}

// A body is not head, so the head cap must not see it.
TEST(RequestReader, BodyBytesDoNotCountTowardTheHeadCap) {
    RequestReader reader;
    const auto result = read(reader, post_body(std::string(max_head_bytes * 2, 'b')));
    ASSERT_TRUE(result) << "unexpected error: " << result.error;
    ASSERT_TRUE(result.request.has_value());
    EXPECT_EQ(result.request->body.size(), max_head_bytes * 2);
}

// The cap is a maximum, not a threshold.
TEST(RequestReader, AcceptsABodyAtExactlyTheCap) {
    RequestReader reader;
    const auto result = read(reader, post_body(std::string(max_body_bytes, 'b')));
    ASSERT_TRUE(result) << "unexpected error: " << result.error;
    ASSERT_TRUE(result.request.has_value());
    EXPECT_EQ(result.request->body.size(), max_body_bytes);
}

// Refused on the declaration alone, so the bytes are never buffered to find out.
TEST(RequestReader, RejectsAContentLengthOverTheCap) {
    EXPECT_EQ(content_length_error(std::to_string(max_body_bytes + 1)), RequestError::BodyTooLarge);
}

// Bounded before the multiply overflows, so it cannot wrap into a small length.
TEST(RequestReader, RejectsAContentLengthTooLargeToHold) {
    EXPECT_EQ(content_length_error("99999999999999999999"), RequestError::BodyTooLarge);
}

TEST(RequestReader, RejectsANonDigitContentLength) {
    for (const std::string_view value : {"abc", "3a", "a3", "+3", "-3", "3.0", "0x3", "3 3"}) {
        EXPECT_EQ(content_length_error(value), RequestError::Malformed) << "value: " << value;
    }
}

// The comma spelling of a repeated field: rejected by the digit rule, not by
// counting, because it arrives as one field.
TEST(RequestReader, RejectsACommaSeparatedContentLength) {
    EXPECT_EQ(content_length_error("3, 3"), RequestError::Malformed);
}

// A present field with no value is a syntax error, unlike an absent one.
TEST(RequestReader, RejectsAnEmptyContentLength) {
    EXPECT_EQ(content_length_error(""), RequestError::Malformed);
}

// Refused even though the two agree: only one framing directive may be believed.
TEST(RequestReader, RejectsARepeatedContentLength) {
    RequestReader reader;
    const auto result = read(
        reader, "POST /p HTTP/1.1\r\nHost: x\r\nContent-Length: 3\r\nContent-Length: 3\r\n\r\nabc");
    EXPECT_EQ(result.error, RequestError::Malformed);
}

TEST(RequestReader, RejectsAnyTransferEncoding) {
    for (const std::string_view coding : {"chunked", "gzip", "identity"}) {
        RequestReader reader;
        std::string bytes = "POST /p HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: ";
        bytes += coding;
        bytes += "\r\n\r\n";
        EXPECT_EQ(read(reader, bytes).error, RequestError::UnsupportedTransferEncoding)
            << "coding: " << coding;
    }
}

// Tested first, so the pair that smuggles a request past one recipient never
// reaches the length rule at all.
TEST(RequestReader, RejectsTransferEncodingAheadOfContentLength) {
    RequestReader reader;
    const auto result =
        read(reader,
             "POST /p HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\nContent-Length: 3\r\n"
             "\r\nabc");
    EXPECT_EQ(result.error, RequestError::UnsupportedTransferEncoding);
}

}  // namespace
