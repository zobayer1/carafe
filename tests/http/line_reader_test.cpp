#include "http/line_reader.hpp"

#include "http/printers.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

namespace {

using carafe::http::LineError;
using carafe::http::LineReader;

using Lines = std::vector<std::string>;

// Mirrors the cap in line_reader.cpp, which tests cannot reach.
constexpr std::size_t max_line_length = 8192;

// Copies out every available line: a view from next_line() dies at the next append().
Lines drain(LineReader& reader) {
    Lines lines;
    for (;;) {
        const auto result = reader.next_line();
        EXPECT_EQ(result.error, LineError::None);
        if (!result.line) {
            return lines;
        }
        lines.emplace_back(*result.line);
    }
}

// take() views into the buffer, which the next append() reallocates: this copies out. Nothing yet stays distinct from
// an empty take, because those differ.
std::optional<std::string> take(LineReader& reader, std::size_t n) {
    const auto bytes = reader.take(n);
    if (!bytes.has_value()) {
        return std::nullopt;
    }
    return std::string{*bytes};
}

// Feeds one byte at a time so every chunk boundary and resumed scan is exercised.
Lines drain_bytewise(LineReader& reader, std::string_view bytes) {
    Lines lines;
    for (const char ch : bytes) {
        reader.append(std::string_view(&ch, 1));
        const auto chunk = drain(reader);
        lines.insert(lines.end(), chunk.begin(), chunk.end());
    }
    return lines;
}

// Nothing buffered means nothing to hand out.
TEST(LineReader, EmptyReaderYieldsNothing) {
    LineReader reader;
    EXPECT_TRUE(drain(reader).empty());
}

// The terminator is consumed, not returned: the parser must never see it.
TEST(LineReader, StripsTheTerminator) {
    LineReader reader;
    reader.append("GET / HTTP/1.1\r\n");
    EXPECT_EQ(drain(reader), (Lines{"GET / HTTP/1.1"}));
}

// One append can hold several lines, and they come out in order.
TEST(LineReader, YieldsEveryLineInOneAppend) {
    LineReader reader;
    reader.append("a\r\nb\r\nc\r\n");
    EXPECT_EQ(drain(reader), (Lines{"a", "b", "c"}));
}

// Engaged-but-empty is the blank line ending a header block, not "nothing yet".
TEST(LineReader, YieldsTheEmptyLine) {
    LineReader reader;
    reader.append("\r\n");
    const auto result = reader.next_line();
    ASSERT_TRUE(result.line.has_value());
    EXPECT_TRUE(result.line->empty());
}

// A line arriving in pieces is reassembled, and nothing is yielded early.
TEST(LineReader, ReassemblesLineSplitAcrossAppends) {
    LineReader reader;
    reader.append("GET / HT");
    EXPECT_TRUE(drain(reader).empty());
    reader.append("TP/1.1\r\n");
    EXPECT_EQ(drain(reader), (Lines{"GET / HTTP/1.1"}));
}

// A trailing CR is not yet a terminator, so the scan must resume behind it.
TEST(LineReader, ReassemblesTerminatorSplitAcrossAppends) {
    LineReader reader;
    reader.append("a\r");
    EXPECT_TRUE(drain(reader).empty());
    reader.append("\n");
    EXPECT_EQ(drain(reader), (Lines{"a"}));
}

// Bytes after the last complete line stay buffered for the next append.
TEST(LineReader, KeepsTrailingPartialLineBuffered) {
    LineReader reader;
    reader.append("a\r\nb");
    EXPECT_EQ(drain(reader), (Lines{"a"}));
    reader.append("\r\n");
    EXPECT_EQ(drain(reader), (Lines{"b"}));
}

// A lone CR stays in the line, where the request-line parser rejects it.
TEST(LineReader, LoneCarriageReturnIsNotATerminator) {
    LineReader reader;
    reader.append("a\rb\r\n");
    EXPECT_EQ(drain(reader), (Lines{"a\rb"}));
}

// RFC 9112 §2.2 permits recognising a bare LF; we decline, so a proxy requiring CRLF cannot disagree with us about
// where a request ends.
TEST(LineReader, BareLineFeedIsNotATerminator) {
    LineReader reader;
    reader.append("a\nb\r\n");
    EXPECT_EQ(drain(reader), (Lines{"a\nb"}));
}

// The whole head of a request delivered one byte per append.
TEST(LineReader, SurvivesBytewiseDelivery) {
    LineReader reader;
    const auto lines = drain_bytewise(reader, "GET / HTTP/1.1\r\nHost: x\r\n\r\n");
    EXPECT_EQ(lines, (Lines{"GET / HTTP/1.1", "Host: x", ""}));
}

// A scan position not shifted along with the buffer loses this line outright.
TEST(LineReader, CompactionPreservesAStraddlingTerminator) {
    LineReader reader;
    reader.append("GET / HTTP/1.1\r\na\r");
    EXPECT_EQ(drain(reader), (Lines{"GET / HTTP/1.1"}));
    reader.append("\n");
    EXPECT_EQ(drain(reader), (Lines{"a"}));
}

// The pending line dwarfs the consumed prefix, so compaction is skipped.
TEST(LineReader, ReassemblesLongLineFollowingAShortOne) {
    LineReader reader;
    const std::string long_line(100, 'x');
    reader.append("a\r\n" + long_line);
    EXPECT_EQ(drain(reader), (Lines{"a"}));
    reader.append("\r\n");
    EXPECT_EQ(drain(reader), (Lines{long_line}));
}

// Compaction fires on nearly every append. Asserts nothing about memory.
TEST(LineReader, SurvivesRepeatedCompaction) {
    LineReader reader;
    for (int i = 0; i < 1000; ++i) {
        reader.append("Host: example.com\r\n");
        EXPECT_EQ(drain(reader), (Lines{"Host: example.com"}));
    }
}

// The cap is a maximum, not a threshold: a line of exactly that length is legal.
TEST(LineReader, AcceptsLineAtExactlyTheCap) {
    LineReader reader;
    const std::string line(max_line_length, 'a');
    reader.append(line + "\r\n");
    EXPECT_EQ(drain(reader), (Lines{line}));
}

// At exactly the cap the legalising CRLF may still be in flight.
TEST(LineReader, HoldsCapLengthLineUntilItsTerminatorArrives) {
    LineReader reader;
    const std::string line(max_line_length, 'a');
    reader.append(line);
    EXPECT_TRUE(drain(reader).empty());
    reader.append("\r\n");
    EXPECT_EQ(drain(reader), (Lines{line}));
}

// Its terminator is found at once, so a cap on the not-found path alone misses it.
TEST(LineReader, RejectsLineOverTheCapInOneAppend) {
    LineReader reader;
    reader.append(std::string(max_line_length + 1, 'a') + "\r\n");
    const auto result = reader.next_line();
    EXPECT_EQ(result.error, LineError::LineTooLong);
    EXPECT_FALSE(result.line.has_value());
}

// A dribbled endless line is rejected, not answered "nothing yet" forever.
TEST(LineReader, RejectsOverlongLineBeforeItsTerminatorArrives) {
    LineReader reader;
    reader.append(std::string(max_line_length + 1, 'a'));
    const auto result = reader.next_line();
    EXPECT_EQ(result.error, LineError::LineTooLong);
    EXPECT_FALSE(result.line.has_value());
}

// There is no resynchronizing from an overlong line: the caller must close.
TEST(LineReader, LineTooLongIsTerminal) {
    LineReader reader;
    reader.append(std::string(max_line_length + 1, 'a'));
    EXPECT_EQ(reader.next_line().error, LineError::LineTooLong);
    EXPECT_EQ(reader.next_line().error, LineError::LineTooLong);
}

// Bytes come out consumed, not peeked at.
TEST(LineReader, TakesExactlyTheRequestedBytes) {
    LineReader reader;
    reader.append("abcdef");
    EXPECT_EQ(take(reader, 3), "abc");
    EXPECT_EQ(take(reader, 3), "def");
}

// A body split across packets is the ordinary case, not a failure.
TEST(LineReader, WaitsForEveryRequestedByte) {
    LineReader reader;
    reader.append("ab");
    EXPECT_EQ(take(reader, 3), std::nullopt);
    reader.append("c");
    EXPECT_EQ(take(reader, 3), "abc");
}

// The order a body actually arrives in: a head read as lines, then raw bytes.
TEST(LineReader, TakesBytesFollowingALine) {
    LineReader reader;
    reader.append("hi\r\nbody");
    EXPECT_EQ(drain(reader), (Lines{"hi"}));
    EXPECT_EQ(take(reader, 4), "body");
}

// Zero bytes are here whatever the buffer holds: a Content-Length of 0 must not be answered "nothing yet" forever.
TEST(LineReader, TakesNothingWithoutWaiting) {
    LineReader reader;
    EXPECT_EQ(take(reader, 0), "");
}

// Taken bytes were never scanned for a terminator. A scan position left behind them finds the CRLF inside them and
// measures the next line from a start that is already past its end.
TEST(LineReader, LineScanningResumesAfterTakenBytes) {
    LineReader reader;
    reader.append("A\r\nB\r\n");
    EXPECT_EQ(take(reader, 3), "A\r\n");
    EXPECT_EQ(drain(reader), (Lines{"B"}));
}

// Compaction shifts the scan position back by the consumed prefix, which underflows if a take left it behind.
TEST(LineReader, SurvivesCompactionAfterATake) {
    LineReader reader;
    reader.append("abcd");
    EXPECT_EQ(take(reader, 2), "ab");
    reader.append("e\r\n");
    EXPECT_EQ(drain(reader), (Lines{"cde"}));
}

// next_line's cap guards a scan for a terminator that may never come. take is told its length up front and has nothing
// to guard against.
TEST(LineReader, TakesMoreThanTheLineLengthCap) {
    LineReader reader;
    const std::string body(max_line_length + 1, 'x');
    reader.append(body);
    EXPECT_EQ(take(reader, body.size()), body);
}

TEST(LineReader, DiscardsExactlyTheRequestedBytes) {
    LineReader reader;
    reader.append("abcdef");
    EXPECT_EQ(reader.discard(3), 3U);
    EXPECT_EQ(take(reader, 3), "def");
}

// Fewer than asked is the ordinary answer rather than a failure: the rest of the body has not arrived yet.
TEST(LineReader, DiscardsOnlyWhatHasArrived) {
    LineReader reader;
    reader.append("ab");
    EXPECT_EQ(reader.discard(5), 2U);
}

TEST(LineReader, DiscardsNothingFromAnEmptyBuffer) {
    LineReader reader;
    EXPECT_EQ(reader.discard(5), 0U);
}

TEST(LineReader, DiscardsNothingWhenAskedForNothing) {
    LineReader reader;
    reader.append("abc");
    EXPECT_EQ(reader.discard(0), 0U);
    EXPECT_EQ(take(reader, 3), "abc");
}

// A refused body arrives in pieces, and the countdown runs across them.
TEST(LineReader, AccumulatesDiscardsAcrossAppends) {
    LineReader reader;
    std::size_t remaining = 10;
    for (const std::string_view chunk : {"abcd", "efgh", "ijkl"}) {
        reader.append(chunk);
        remaining -= reader.discard(remaining);
    }
    EXPECT_EQ(remaining, 0U);
    EXPECT_EQ(take(reader, 2), "kl");
}

// Discarded bytes were never scanned, so a scan position left behind them finds a terminator inside them.
TEST(LineReader, LineScanningResumesAfterDiscardedBytes) {
    LineReader reader;
    reader.append("A\r\nB\r\n");
    EXPECT_EQ(reader.discard(3), 3U);
    EXPECT_EQ(drain(reader), (Lines{"B"}));
}

// Compaction shifts the scan position back by the consumed prefix, which underflows if a discard left it behind.
TEST(LineReader, SurvivesCompactionAfterADiscard) {
    LineReader reader;
    reader.append("abcd");
    EXPECT_EQ(reader.discard(2), 2U);
    reader.append("e\r\n");
    EXPECT_EQ(drain(reader), (Lines{"cde"}));
}

// The whole point of dropping a body: what follows it is the next request.
TEST(LineReader, LeavesTheNextRequestLineIntact) {
    LineReader reader;
    reader.append("bodybytesGET / HTTP/1.1\r\n");
    EXPECT_EQ(reader.discard(9), 9U);
    EXPECT_EQ(drain(reader), (Lines{"GET / HTTP/1.1"}));
}

}  // namespace
