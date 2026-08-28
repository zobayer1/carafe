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

// Mirror the caps in request_reader.cpp and line_reader.cpp, which tests cannot reach. Crossing each from both sides
// keeps drift from being silent.
constexpr std::size_t max_head_bytes = 65536;
constexpr std::size_t max_header_fields = 100;
constexpr std::size_t max_line_length = 8192;
constexpr std::size_t max_body_bytes = 1048576;
constexpr std::size_t max_drain_bytes = 8388608;

// One append, one answer.
RequestResult read(RequestReader& reader, std::string_view bytes) {
    reader.append(bytes);
    return reader.next_request();
}

// Feeds one byte at a time, stopping at the first completed request or failure, so every chunk boundary in the head is
// exercised.
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
    return "POST /p HTTP/1.1\r\nHost: x\r\nContent-Length: " + std::to_string(body.size()) + "\r\n\r\n" +
           std::string{body};
}

// One chunk: its size in lowercase hex, then its bytes, each closed by CRLF.
std::string chunk(std::string_view data) {
    constexpr std::string_view hex_digits = "0123456789abcdef";
    std::string size;
    for (std::size_t left = data.size(); left > 0; left /= 16) {
        size.insert(size.begin(), hex_digits.at(left % 16));
    }
    if (size.empty()) {
        size = "0";
    }
    return size + "\r\n" + std::string{data} + "\r\n";
}

// The zero-size chunk and an empty trailer section, which is what ends a chunked body.
constexpr std::string_view last_chunk = "0\r\n\r\n";

// A POST framed by chunked coding, carrying `body` between the head and whatever ends it.
std::string post_chunked(std::string_view body) {
    return "POST /p HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n" + std::string{body};
}

// A POST whose Content-Length carries `value`, with no body following.
RequestResult content_length_result(std::string_view value) {
    RequestReader reader;
    std::string bytes = "POST /p HTTP/1.1\r\nHost: x\r\nContent-Length: ";
    bytes += value;
    bytes += "\r\n\r\n";
    return read(reader, bytes);
}

// Feeds `length` body bytes in socket-sized pieces, checking the reader stays mid-drain the whole way: no request, and
// no failure.
void feed_body(RequestReader& reader, std::size_t length) {
    constexpr std::size_t chunk = 4096;
    for (std::size_t sent = 0; sent < length; sent += chunk) {
        reader.append(std::string(sent + chunk > length ? length - sent : chunk, 'b'));
        const auto pending = reader.next_request();
        EXPECT_TRUE(pending) << "unexpected error: " << pending.error;
        EXPECT_FALSE(pending.request.has_value());
    }
}

// `count` field lines, each value `value_size` bytes, with distinct names.
std::string pad_fields(std::size_t count, std::size_t value_size) {
    std::string out;
    for (std::size_t i = 0; i < count; ++i) {
        out += "x-pad-" + std::to_string(i) + ": " + std::string(value_size, 'v') + "\r\n";
    }
    return out;
}

// A padding field of exactly `value_size + pad_field_overhead` wire bytes. The index is zero-padded so the size does
// not depend on how many came before it.
constexpr std::size_t pad_field_overhead = 13;  // "x-pad-NNN: " + CRLF

std::string pad_field(std::size_t index, std::size_t value_size) {
    std::string digits = std::to_string(index);
    digits.insert(digits.begin(), 3 - digits.size(), '0');
    return "x-pad-" + digits + ": " + std::string(value_size, 'v') + "\r\n";
}

// A complete head whose wire size, every terminator included, is exactly `bytes`. Padding is split across several
// fields because no single line may exceed max_line_length.
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

// The whole head delivered one byte per append, which is the shape hostile input takes and the case every resumed scan
// has to survive.
TEST(RequestReader, SurvivesBytewiseDelivery) {
    RequestReader reader;
    const auto result = read_bytewise(reader, "GET /a?b=c HTTP/1.1\r\nHost: x\r\nAccept: */*\r\n\r\n");
    ASSERT_TRUE(result) << "unexpected error: " << result.error;
    ASSERT_TRUE(result.request.has_value());
    EXPECT_EQ(result.request->target, "/a?b=c");
    EXPECT_EQ(result.request->headers.get("accept"), "*/*");
}

// The blank line ends the head. Until it arrives there is no request, and that must not read as a failure.
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

// Completion resets rather than terminating, so a pipelined connection yields its requests one call at a time from
// bytes that arrived together.
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

// The per-request budgets reset with the phase. Without that, a long-lived connection starts rejecting requests that
// are individually well within both caps. No branch is missing, so only feeding two requests finds it.
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

// One LineError::LineTooLong, two outcomes: which one depends on the phase, and resolving that ambiguity is why this
// class exists.
TEST(RequestReader, DistinguishesOverlongRequestLineFromOverlongField) {
    RequestReader reader1;
    const std::string long_target(max_line_length, 'a');
    EXPECT_EQ(read(reader1, "GET /" + long_target + " HTTP/1.1\r\n").error, RequestError::RequestLineTooLong);

    RequestReader reader2;
    const std::string long_value(max_line_length, 'v');
    EXPECT_EQ(read(reader2, "GET / HTTP/1.1\r\nX-Long: " + long_value + "\r\n").error, RequestError::HeaderTooLong);
}

// A field count cap the byte cap cannot supply: these fields are tiny, so only counting them catches this.
TEST(RequestReader, RejectsTooManyFields) {
    RequestReader reader;
    const auto result = read(reader, "GET / HTTP/1.1\r\nHost: x\r\n" + pad_fields(max_header_fields, 1) + "\r\n");
    EXPECT_EQ(result.error, RequestError::TooManyHeaders);
}

// Exactly at the cap is legal; Host is one of the counted fields.
TEST(RequestReader, AcceptsFieldsUpToTheCap) {
    RequestReader reader;
    const auto result = read(reader, "GET / HTTP/1.1\r\nHost: x\r\n" + pad_fields(max_header_fields - 1, 1) + "\r\n");
    ASSERT_TRUE(result) << "unexpected error: " << result.error;
    ASSERT_TRUE(result.request.has_value());
    EXPECT_EQ(result.request->headers.size(), max_header_fields);
}

// The cap is a maximum, not a threshold. Few enough fields that the count cap cannot be what accepts or rejects these
// two.
TEST(RequestReader, AcceptsHeadAtExactlyTheCap) {
    const std::string head = head_of_size(max_head_bytes);
    ASSERT_EQ(head.size(), max_head_bytes);

    RequestReader reader;
    const auto result = read(reader, head);
    ASSERT_TRUE(result) << "unexpected error: " << result.error;
    EXPECT_TRUE(result.request.has_value());
}

// One byte more, and it is 431. Being exact is what pins the accounting: an implementation that forgot to count the
// terminators lands under the cap here and accepts.
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

// There is no resynchronising a broken head: the caller must close, and feeding more bytes must not produce a request.
TEST(RequestReader, FailureIsTerminal) {
    RequestReader reader;
    EXPECT_EQ(read(reader, "BOGUS\r\n").error, RequestError::Malformed);

    const auto after = read(reader, "GET / HTTP/1.1\r\nHost: x\r\n\r\n");
    EXPECT_EQ(after.error, RequestError::Malformed);
    EXPECT_FALSE(after.request.has_value());
    EXPECT_FALSE(after.stream_continues);
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
    const auto pending = read(reader, "POST /p HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\n\r\nhel");
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

// Unconsumed body bytes parse as the next request line, so an ordinary GET behind a body comes back 501. Reading the
// body to its end is what keeps the two apart.
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

// Framing does not consult the method: five bytes are five bytes whatever the verb means by them.
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

// Refused on the declaration alone, so the bytes are never buffered to find out. The length is known, so the reader can
// still read past them.
TEST(RequestReader, RejectsAContentLengthOverTheCap) {
    const auto result = content_length_result(std::to_string(max_body_bytes + 1));
    EXPECT_EQ(result.error, RequestError::BodyTooLarge);
    EXPECT_TRUE(result.stream_continues);
}

// Bounded before the multiply overflows, so it cannot wrap into a small length. Nothing usable came back, so there is
// no length to drain and no way to resume.
TEST(RequestReader, RejectsAContentLengthTooLargeToHold) {
    const auto result = content_length_result("99999999999999999999");
    EXPECT_EQ(result.error, RequestError::BodyTooLarge);
    EXPECT_FALSE(result.stream_continues);
}

TEST(RequestReader, RejectsANonDigitContentLength) {
    for (const std::string_view value : {"abc", "3a", "a3", "+3", "-3", "3.0", "0x3", "3 3"}) {
        EXPECT_EQ(content_length_result(value).error, RequestError::Malformed) << "value: " << value;
    }
}

// The comma spelling of a repeated field: rejected by the digit rule, not by counting, because it arrives as one field.
TEST(RequestReader, RejectsACommaSeparatedContentLength) {
    EXPECT_EQ(content_length_result("3, 3").error, RequestError::Malformed);
}

// A present field with no value is a syntax error, unlike an absent one.
TEST(RequestReader, RejectsAnEmptyContentLength) {
    EXPECT_EQ(content_length_result("").error, RequestError::Malformed);
}

// Refused even though the two agree: only one framing directive may be believed.
TEST(RequestReader, RejectsARepeatedContentLength) {
    RequestReader reader;
    const auto result =
        read(reader, "POST /p HTTP/1.1\r\nHost: x\r\nContent-Length: 3\r\nContent-Length: 3\r\n\r\nabc");
    EXPECT_EQ(result.error, RequestError::Malformed);
}

// RFC 9112 §6.1 and §6.3 give three answers, not one. Chunked last means the body's end is findable, so a coding under
// it is only undecodable; chunked anywhere else, or twice, means the end cannot be found at all.
TEST(RequestReader, ClassifiesTheTransferCodings) {
    struct Case {
        std::string_view coding;
        RequestError error;
    };

    for (const Case test :
         {Case{"chunked", RequestError::None}, Case{"gzip, chunked", RequestError::UnsupportedTransferEncoding},
          Case{"chunked, gzip", RequestError::Malformed}, Case{"gzip", RequestError::Malformed},
          Case{"chunked, chunked", RequestError::Malformed}, Case{"", RequestError::Malformed}}) {
        RequestReader reader;
        std::string bytes = "POST /p HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: ";
        bytes += test.coding;
        bytes += "\r\n\r\n";
        bytes += last_chunk;
        EXPECT_EQ(read(reader, bytes).error, test.error) << "coding: " << test.coding;
    }
}

// §5.3 again: one coding list may be spelt across several fields, and which field a coding sits in must not change
// the answer. Reading only the first is how a recipient concludes "chunked" from a list that ends in something else.
TEST(RequestReader, FoldsRepeatedTransferEncodingFields) {
    RequestReader undecodable;
    EXPECT_EQ(read(undecodable,
                   "POST /p HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: gzip\r\n"
                   "Transfer-Encoding: chunked\r\n\r\n0\r\n\r\n")
                  .error,
              RequestError::UnsupportedTransferEncoding);

    RequestReader unframeable;
    EXPECT_EQ(read(unframeable,
                   "POST /p HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n"
                   "Transfer-Encoding: gzip\r\n\r\n0\r\n\r\n")
                  .error,
              RequestError::Malformed);
}

// §6.1: a 1.0 client cannot know the next hop speaks 1.1, so honouring a chunked body from one desynchronises it.
TEST(RequestReader, RejectsChunkedFromAnHttpTenClient) {
    RequestReader reader;
    const auto result = read(reader, "POST /p HTTP/1.0\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n\r\n");
    EXPECT_EQ(result.error, RequestError::Malformed);
    EXPECT_FALSE(result.stream_continues);
}

// RFC 9112 §6.3 rule 3: the pair is refused before either field is read for framing, and the coding does not matter.
// One recipient preferring the length and the next preferring the coding is exactly how a request gets smuggled.
TEST(RequestReader, RejectsTransferEncodingAlongsideContentLength) {
    for (const std::string_view coding : {"chunked", "gzip"}) {
        RequestReader reader;
        std::string bytes = "POST /p HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: ";
        bytes += coding;
        bytes += "\r\nContent-Length: 3\r\n\r\nabc";
        const auto result = read(reader, bytes);
        EXPECT_EQ(result.error, RequestError::Malformed) << "coding: " << coding;
        EXPECT_FALSE(result.stream_continues) << "coding: " << coding;
    }
}

// Past the drain ceiling the length is known and refused anyway: reading that much only to throw it away costs more
// than the connection is worth.
TEST(RequestReader, EndsTheStreamOnABodyTooLargeToDrain) {
    const auto result = content_length_result(std::to_string(max_drain_bytes + 1));
    EXPECT_EQ(result.error, RequestError::BodyTooLarge);
    EXPECT_FALSE(result.stream_continues);
}

// The refused body is dropped and what follows it is an ordinary request, with none of the refused head's fields
// carried into it.
TEST(RequestReader, DrainsARefusedBodyAndReadsTheNext) {
    RequestReader reader;
    reader.append(post_body(std::string(max_body_bytes + 1, 'b')) + "GET /b HTTP/1.1\r\nHost: y\r\n\r\n");

    const auto refused = reader.next_request();
    EXPECT_EQ(refused.error, RequestError::BodyTooLarge);
    EXPECT_TRUE(refused.stream_continues);

    const auto next = reader.next_request();
    ASSERT_TRUE(next.request.has_value()) << "unexpected error: " << next.error;
    EXPECT_EQ(next.request->target, "/b");
    EXPECT_EQ(next.request->headers.size(), 1U);
    EXPECT_EQ(next.request->headers.get("host"), "y");
    EXPECT_TRUE(next.request->body.empty());
}

// A megabyte does not arrive at once, so the countdown has to span reads.
TEST(RequestReader, DrainsARefusedBodyAcrossAppends) {
    constexpr std::size_t length = max_body_bytes + 1;

    RequestReader reader;
    reader.append("POST /p HTTP/1.1\r\nHost: x\r\nContent-Length: " + std::to_string(length) + "\r\n\r\n");
    EXPECT_EQ(reader.next_request().error, RequestError::BodyTooLarge);

    feed_body(reader, length);

    reader.append("GET /b HTTP/1.1\r\nHost: y\r\n\r\n");
    const auto next = reader.next_request();
    ASSERT_TRUE(next.request.has_value()) << "unexpected error: " << next.error;
    EXPECT_EQ(next.request->target, "/b");
}

// Everything else leaves the reader with no idea where the next request starts.
TEST(RequestReader, EndsTheStreamOnAFailureItCannotReadPast) {
    for (const std::string_view head :
         {"NOTAREQUEST\r\n\r\n", "GET / HTTP/1.1\r\nHost: x\r\nbad header\r\n\r\n", "GET / HTTP/2.0\r\nHost: x\r\n\r\n",
          "FROB / HTTP/1.1\r\nHost: x\r\n\r\n", "POST /p HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: gzip\r\n\r\n",
          "POST /p HTTP/1.1\r\nHost: x\r\nContent-Length: abc\r\n\r\n"}) {
        RequestReader reader;
        const auto result = read(reader, head);
        EXPECT_FALSE(result) << "expected a failure for: " << head;
        EXPECT_FALSE(result.stream_continues) << "head: " << head;
    }
}

// The version outlives the failure: a refusal hands over no Request, and the caller still has to decide whether the
// client is waiting for a close.
TEST(RequestReader, ReportsTheVersionOfARefusedRequest) {
    const std::string length = std::to_string(max_body_bytes + 1);

    RequestReader ten;
    ten.append("POST /p HTTP/1.0\r\nContent-Length: " + length + "\r\n\r\n");
    const auto refused_ten = ten.next_request();
    EXPECT_EQ(refused_ten.error, RequestError::BodyTooLarge);
    EXPECT_EQ(refused_ten.version, Version::Http10);

    RequestReader eleven;
    eleven.append("POST /p HTTP/1.1\r\nHost: x\r\nContent-Length: " + length + "\r\n\r\n");
    const auto refused_eleven = eleven.next_request();
    EXPECT_EQ(refused_eleven.error, RequestError::BodyTooLarge);
    EXPECT_EQ(refused_eleven.version, Version::Http11);
}

// A latched failure repeats its whole answer, version included: the caller may ask again before it decides what to
// write.
TEST(RequestReader, KeepsTheVersionOnARepeatedFailure) {
    RequestReader reader;
    reader.append("POST /p HTTP/1.0\r\nTransfer-Encoding: chunked\r\n\r\n");

    const auto first = reader.next_request();
    EXPECT_EQ(first.error, RequestError::Malformed);
    EXPECT_EQ(first.version, Version::Http10);

    const auto again = reader.next_request();
    EXPECT_EQ(again.error, RequestError::Malformed);
    EXPECT_EQ(again.version, Version::Http10);
}

// A body arriving in pieces that declare their own sizes, with no total anywhere: the end is findable only from the
// chunks themselves.
TEST(RequestReader, ReadsAChunkedBody) {
    RequestReader reader;
    const auto result = read(reader, post_chunked(chunk("hello") + chunk(" world") + std::string{last_chunk}));

    ASSERT_TRUE(result.request.has_value()) << "unexpected error: " << result.error;
    EXPECT_EQ(result.request->body, "hello world");
}

// Every boundary inside a chunked body, including mid-size-line and mid-chunk, since none of them arrive aligned.
TEST(RequestReader, ReadsAChunkedBodyAcrossAppends) {
    RequestReader reader;
    const auto result = read_bytewise(reader, post_chunked(chunk("hello") + chunk(" world") + std::string{last_chunk}));

    ASSERT_TRUE(result.request.has_value()) << "unexpected error: " << result.error;
    EXPECT_EQ(result.request->body, "hello world");
}

// A declared zero and a body of no chunks are the same request, and neither carries bytes.
TEST(RequestReader, ReadsAnEmptyChunkedBody) {
    RequestReader reader;
    const auto result = read(reader, post_chunked(std::string{last_chunk}));

    ASSERT_TRUE(result.request.has_value()) << "unexpected error: " << result.error;
    EXPECT_TRUE(result.request->body.empty());
}

// The same guarantee Content-Length bodies get: the reader knows where this body ended, so the next request starts
// where it says it does rather than inside the leftovers.
TEST(RequestReader, ReadsARequestPipelinedBehindAChunkedBody) {
    RequestReader reader;
    reader.append(post_chunked(chunk("abc") + std::string{last_chunk}) + "GET /b HTTP/1.1\r\nHost: y\r\n\r\n");

    const auto first = reader.next_request();
    ASSERT_TRUE(first.request.has_value()) << "unexpected error: " << first.error;
    EXPECT_EQ(first.request->body, "abc");

    const auto next = reader.next_request();
    ASSERT_TRUE(next.request.has_value()) << "unexpected error: " << next.error;
    EXPECT_EQ(next.request->target, "/b");
    EXPECT_TRUE(next.request->body.empty());
}

// RFC 9112 §7.1.1: an extension is there to be ignored, and ignoring it must not shift where the size ends.
TEST(RequestReader, IgnoresAChunkExtension) {
    RequestReader reader;
    const auto result = read(reader, post_chunked("5;name=value\r\nhello\r\n" + std::string{last_chunk}));

    ASSERT_TRUE(result.request.has_value()) << "unexpected error: " << result.error;
    EXPECT_EQ(result.request->body, "hello");
}

// §7.1: the CRLF after the data is framing, not data. A chunk without it has lied about its size, and believing the
// size anyway is what leaves the next request starting mid-body.
TEST(RequestReader, RejectsAChunkThatDoesNotEndInCrlf) {
    RequestReader reader;
    const auto result = read(reader, post_chunked("5\r\nhelloXX" + std::string{last_chunk}));

    EXPECT_EQ(result.error, RequestError::Malformed);
    EXPECT_FALSE(result.stream_continues);
}

// §7.1: chunk-size = 1*HEXDIG, whole. BWS is refused too: senders must not generate it, and whitespace a framing
// parser forgives is how two recipients disagree about where a body ends.
TEST(RequestReader, RejectsANonHexChunkSize) {
    for (const std::string_view size : {"z", "5x", "-1", "0x5", "+5", "", "5 ", " 5"}) {
        RequestReader reader;
        std::string bytes = post_chunked("");
        bytes += size;
        bytes += "\r\nhello\r\n";
        EXPECT_EQ(read(reader, bytes).error, RequestError::Malformed) << "size: " << size;
    }
}

// Refused on the size line, before a byte of the chunk is buffered, the way a Content-Length over the cap is.
TEST(RequestReader, RejectsAChunkSizeOverTheCap) {
    RequestReader reader;
    const auto result = read(reader, post_chunked("100001\r\n"));

    EXPECT_EQ(result.error, RequestError::BodyTooLarge);
}

// Bounded before the multiply overflows, so a size can never wrap into a smaller one. 2^64 wraps to zero and would end
// the body early; one digit further and it wraps to a short chunk, leaving the rest of the body to parse as a request.
// The cumulative cap cannot catch either, because by then the number is already small.
TEST(RequestReader, RejectsAChunkSizeTooLargeToHold) {
    for (const std::string_view size : {"10000000000000000", "10000000000000005", "ffffffffffffffffff"}) {
        RequestReader reader;
        std::string bytes = post_chunked("");
        bytes += size;
        bytes += "\r\nhello\r\n";
        EXPECT_EQ(read(reader, bytes).error, RequestError::BodyTooLarge) << "size: " << size;
    }
}

// No total is ever declared, so the cap has to be applied as the body grows. Two chunks, each legal alone.
TEST(RequestReader, RejectsChunksThatAddPastTheCap) {
    const std::string half((max_body_bytes / 2) + 1, 'b');

    RequestReader reader;
    reader.append(post_chunked(chunk(half) + chunk(half)));
    const auto result = reader.next_request();

    EXPECT_EQ(result.error, RequestError::BodyTooLarge);
    EXPECT_FALSE(result.stream_continues);
}

// An over-long size line is a broken body, not a header block. Reporting 431 here would tell a client its fields were
// too large when it sent two.
TEST(RequestReader, ReportsAnOverlongChunkSizeLineAsMalformed) {
    RequestReader reader;
    const auto result = read(reader, post_chunked(std::string(max_line_length + 1, '0') + "\r\n"));

    EXPECT_EQ(result.error, RequestError::Malformed);
}

// Chunk-size lines are body framing, not head. Charging them to the head cap rejected an upload a fiftieth of the size
// the body cap allows, purely for arriving in small pieces.
TEST(RequestReader, KeepsChunkSizeLinesOutOfTheHeadCap) {
    constexpr std::size_t chunks = 22000;
    std::string body;
    for (std::size_t i = 0; i < chunks; ++i) {
        body += chunk("x");
    }

    RequestReader reader;
    reader.append(post_chunked(body + std::string{last_chunk}));
    const auto result = reader.next_request();

    ASSERT_TRUE(result.request.has_value()) << "unexpected error: " << result.error;
    EXPECT_EQ(result.request->body.size(), chunks);
}

// RFC 9112 §7.1.2: the trailer section is read and dropped. Merging one into the head would put fields there after the
// head has already been validated, which is the whole reason a trailer is worth smuggling.
TEST(RequestReader, DropsTheTrailerSection) {
    RequestReader reader;
    const auto result = read(reader, post_chunked(chunk("hello") + "0\r\nX-Trailer: v\r\nX-Another: w\r\n\r\n"));

    ASSERT_TRUE(result.request.has_value()) << "unexpected error: " << result.error;
    EXPECT_EQ(result.request->body, "hello");
    EXPECT_FALSE(result.request->headers.contains("x-trailer"));
    EXPECT_EQ(result.request->headers.size(), 2U);
}

// Dropped, but not unread: a trailer section that is not field lines is a framing error like any other.
TEST(RequestReader, RejectsAMalformedTrailerField) {
    RequestReader reader;
    const auto result = read(reader, post_chunked(chunk("hi") + "0\r\nnot a field\r\n\r\n"));

    EXPECT_EQ(result.error, RequestError::Malformed);
}

// The same cap the head block carries, for the same reason: dropping a field still costs the read that found it.
TEST(RequestReader, RejectsTooManyTrailerFields) {
    std::string trailers = "0\r\n";
    for (std::size_t i = 0; i <= max_header_fields; ++i) {
        trailers += "X-Filler-" + std::to_string(i) + ": v\r\n";
    }
    trailers += "\r\n";

    RequestReader reader;
    const auto result = read(reader, post_chunked(chunk("hi") + trailers));

    EXPECT_EQ(result.error, RequestError::TooManyHeaders);
}

}  // namespace
