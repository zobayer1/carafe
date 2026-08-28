#include "http/request_reader.hpp"

#include <carafe/http/headers.hpp>
#include <carafe/http/request.hpp>

#include "http/ascii.hpp"
#include "http/field_list.hpp"
#include "http/line_reader.hpp"
#include "http/request_parser.hpp"

#include <cstddef>
#include <limits>
#include <optional>
#include <string_view>
#include <utility>

namespace carafe::http {

// Request line plus every field line: the per-line cap bounds one field, this bounds how many a client may stack up.
constexpr std::size_t max_head_bytes = 65536;

// Bytes alone would admit thousands of tiny fields, whose vector and string overhead costs far more than their bytes
// suggest.
constexpr std::size_t max_fields = 100;

// Stripped by LineReader, but still bytes the client sent.
constexpr std::size_t crlf_size = 2;

// No streaming yet, so a body is held whole in memory before a handler sees it.
constexpr std::size_t max_body_bytes = 1048576;

// Past this, reading a refused body and throwing it away costs more than the connection is worth, so the refusal closes
// instead.
constexpr std::size_t max_drain_bytes = 8388608;

// For CRLF sequences.
constexpr std::string_view crlf{"\r\n"};

// The hex counterpart of parse_content_length's bound: tested after each digit, so the next multiply starts below the
// cap and cannot wrap.
static_assert(max_body_bytes <= (std::numeric_limits<std::size_t>::max() - 15) / 16);

// What lets the digit loop carry one bound instead of two: the cap is tested after each digit, so the next multiply
// starts below it and cannot wrap.
static_assert(max_drain_bytes <= (std::numeric_limits<std::size_t>::max() - 9) / 10);

namespace {

struct BodyLength {
    RequestError error = RequestError::None;
    std::size_t bytes = 0;

    // Chunked framing, which makes `bytes` meaningless: the length is not known until the last chunk arrives.
    bool chunked = false;
};

struct Codings {
    std::size_t count = 0;
    std::size_t chunked = 0;
    std::string_view last;
};

struct ChunkLength {
    RequestError error = RequestError::None;
    std::size_t bytes = 0;
};

// RFC 9110 §5.3: several Transfer-Encoding fields are one list, so they are walked as one.
[[nodiscard]] Codings transfer_codings(const Headers& headers) noexcept {
    Codings codings;
    for (const auto& field : headers) {
        if (field.name != "transfer-encoding") {
            continue;
        }
        std::size_t pos = 0;
        while (const auto coding_view = next_list_element(field.value, pos)) {
            ++codings.count;
            codings.chunked += static_cast<std::size_t>(ascii_equals_lowered("chunked", *coding_view));
            codings.last = *coding_view;
        }
    }
    return codings;
}

// RFC 9112 §6.2: Content-Length = 1*DIGIT. The whole value, not a leading number: reading "3, 3" as 3 frames the
// request the way only one of two disagreeing recipients would.
[[nodiscard]] BodyLength parse_content_length(std::string_view value) noexcept {
    if (value.empty()) {
        return {RequestError::Malformed, 0};
    }

    std::size_t bytes = 0;
    for (const char ch : value) {
        const auto digit = static_cast<unsigned char>(ch);
        if (digit < '0' || digit > '9') {
            return {RequestError::Malformed, 0};
        }
        bytes = (bytes * 10) + (digit - '0');
        if (bytes > max_drain_bytes) {
            return {RequestError::BodyTooLarge, 0};
        }
    }
    return {RequestError::None, bytes};
}

// RFC 9112 §7.1: chunk-size = 1*HEXDIG, and §7.1.1 puts an optional chunk-ext after it that a recipient may ignore, so
// the size ends at the first ";". No whitespace is tolerated on either side: senders MUST NOT generate BWS, and
// whitespace a framing parser forgives is how two recipients come to disagree about where a body ends.
[[nodiscard]] ChunkLength parse_chunk_size(std::string_view line) noexcept {
    const auto ext = line.find(';');
    if (ext != std::string_view::npos) {
        line = line.substr(0, ext);
    }

    if (line.empty()) {
        return {RequestError::Malformed, 0};
    }

    std::size_t bytes = 0;
    for (const char ch : line) {
        const int digit = ascii_hex_value(ch);
        if (digit < 0) {
            return {RequestError::Malformed, 0};
        }
        bytes = (bytes * 16) + static_cast<std::size_t>(digit);
        if (bytes > max_body_bytes) {
            return {RequestError::BodyTooLarge, 0};
        }
    }
    return {RequestError::None, bytes};
}

// RFC 9112 §6.1 and §6.3, minus the rules only a response can reach.
[[nodiscard]] BodyLength measure_body(const Headers& headers, Version version) noexcept {
    if (headers.contains("transfer-encoding")) {
        // §6.3 rule 3: the pair is the smuggling vector, and two recipients resolving it differently is the exploit.
        if (headers.contains("content-length")) {
            return {RequestError::Malformed, 0};
        }

        // §6.1: a 1.0 client cannot know the next hop speaks 1.1, so a chunked body from one desynchronises it.
        if (version == Version::Http10) {
            return {RequestError::Malformed, 0};
        }

        const Codings codings = transfer_codings(headers);

        // §6.1: chunked last or the body's end cannot be found, and never twice.
        if (!ascii_equals_lowered("chunked", codings.last) || codings.chunked > 1) {
            return {RequestError::Malformed, 0};
        }

        // Chunked is final, so framing is knowable, but something under it is not ours to decode.
        if (codings.count > 1) {
            return {RequestError::UnsupportedTransferEncoding, 0};
        }

        return {RequestError::None, 0, true};
    }

    // Refused even when the values agree: §5.3 makes this and "3, 3" the same request, and recipients resolving them
    // differently is how one gets smuggled.
    if (headers.count("content-length") > 1) {
        return {RequestError::Malformed, 0};
    }

    const auto value = headers.get("content-length");

    // §6.3: no Content-Length on a request means no body. Running to end of stream is a response reading.
    if (!value.has_value()) {
        return {};
    }

    return parse_content_length(*value);
}

}  // namespace

void RequestReader::arm_next_request() {
    phase_ = Phase::RequestLine;
    request_ = Request{};
    head_bytes_ = 0;
    body_bytes_ = 0;
    field_count_ = 0;
}

RequestResult RequestReader::finish_request() {
    Request ready = std::move(request_);

    arm_next_request();

    return {RequestError::None, std::move(ready)};
}

RequestResult RequestReader::fail(RequestError error) {
    failure_ = error;
    return {error, std::nullopt, false, request_.version};
}

RequestResult RequestReader::refuse(RequestError error) {
    phase_ = Phase::Discard;
    return {error, std::nullopt, true, request_.version};
}

void RequestReader::append(std::string_view bytes) {
    line_reader_.append(bytes);
}

std::optional<RequestResult> RequestReader::handle_request_line(std::string_view line) {
    RequestLineResult parsed = parse_request_line(line);

    // No default: a new ParseError has to break this build, not become a 400.
    switch (parsed.error) {
        case ParseError::None:
            break;
        case ParseError::Malformed:
            return fail(RequestError::Malformed);
        case ParseError::UnknownMethod:
            return fail(RequestError::UnknownMethod);
        case ParseError::UnsupportedVersion:
            return fail(RequestError::UnsupportedVersion);
    }

    request_.method = parsed.value.method;
    request_.target = std::move(parsed.value.target);
    request_.version = parsed.value.version;
    phase_ = Phase::Headers;
    return std::nullopt;
}

std::optional<RequestResult> RequestReader::handle_field_line(std::string_view line) {
    if (line.empty()) {
        return complete_head();
    }

    // Counted before parsing, so a flood costs the attacker more than it costs us.
    if (++field_count_ > max_fields) {
        return fail(RequestError::TooManyHeaders);
    }

    auto field = parse_header_field(line);
    if (!field.has_value()) {
        return fail(RequestError::Malformed);
    }

    request_.headers.add(std::move(*field));
    return std::nullopt;
}

std::optional<RequestResult> RequestReader::complete_head() {
    // RFC 9112 §3.2 requires exactly one Host on 1.1. Two are ambiguous at any version, and that ambiguity is a routing
    // decision an attacker would make.
    const auto host_count = request_.headers.count("host");
    if (host_count > 1 || (host_count == 0 && request_.version == Version::Http11)) {
        return fail(RequestError::Malformed);
    }

    const BodyLength body = measure_body(request_.headers, request_.version);
    if (body.error != RequestError::None) {
        return fail(body.error);
    }
    if (body.chunked) {
        phase_ = Phase::ChunkSize;
        return std::nullopt;
    }
    body_bytes_ = body.bytes;

    // Over the limit but inside the drain ceiling: known, and small enough to drop.
    if (body_bytes_ > max_body_bytes) {
        return refuse(RequestError::BodyTooLarge);
    }

    if (body_bytes_ == 0) {
        return finish_request();
    }

    phase_ = Phase::Body;
    return std::nullopt;
}

std::optional<RequestResult> RequestReader::handle_chunk_size(std::string_view line) {
    const ChunkLength chunk = parse_chunk_size(line);
    if (chunk.error != RequestError::None) {
        return fail(chunk.error);
    }

    // §7.1: a zero size is the last chunk, and what follows is the trailer section rather than more body.
    if (chunk.bytes == 0) {
        phase_ = Phase::Trailers;
        return std::nullopt;
    }

    // What has arrived plus what this chunk promises: no total was ever declared, so the cap is checked as it grows.
    if (request_.body.size() + chunk.bytes > max_body_bytes) {
        return fail(RequestError::BodyTooLarge);
    }

    chunk_bytes_ = chunk.bytes;
    phase_ = Phase::ChunkData;
    return std::nullopt;
}

std::optional<RequestResult> RequestReader::handle_trailer_line(std::string_view line) {
    // §7.1.2: the blank line ends the trailer section, and the request with it.
    if (line.empty()) {
        return finish_request();
    }

    if (++field_count_ > max_fields) {
        return fail(RequestError::TooManyHeaders);
    }

    // Parsed to reject a malformed one, then dropped: the head was validated already, and a trailer merged into it
    // afterwards can contradict what that validation concluded.
    if (!parse_header_field(line).has_value()) {
        return fail(RequestError::Malformed);
    }
    return std::nullopt;
}

std::optional<RequestResult> RequestReader::read_body_bytes() {
    if (phase_ == Phase::Discard) {
        body_bytes_ -= line_reader_.discard(body_bytes_);
        if (body_bytes_ > 0) {
            return RequestResult{};
        }
        arm_next_request();
        return std::nullopt;
    }

    if (phase_ == Phase::Body) {
        const auto body = line_reader_.take(body_bytes_);
        if (!body.has_value()) {
            return RequestResult{};
        }
        request_.body.assign(*body);
        return finish_request();
    }

    const auto chunk = line_reader_.take(chunk_bytes_ + crlf_size);
    if (!chunk.has_value()) {
        return RequestResult{};
    }

    // §7.1: every chunk ends with its own CRLF, and a chunk that does not is a framing error rather than data.
    if (chunk->substr(chunk_bytes_) != crlf) {
        return fail(RequestError::Malformed);
    }

    request_.body.append(chunk->substr(0, chunk_bytes_));
    phase_ = Phase::ChunkSize;
    return std::nullopt;
}

std::optional<RequestResult> RequestReader::read_next_line() {
    const LineResult line_res = line_reader_.next_line();

    // One LineError, several answers: only this layer knows which line it asked for.
    if (line_res.error == LineError::LineTooLong) {
        switch (phase_) {
            case Phase::RequestLine:
                return fail(RequestError::RequestLineTooLong);
            case Phase::Headers:
            case Phase::Trailers:
                return fail(RequestError::HeaderTooLong);
            case Phase::ChunkSize:
            case Phase::Body:
            case Phase::Discard:
            case Phase::ChunkData:
                break;
        }
        return fail(RequestError::Malformed);
    }

    if (!line_res.line.has_value()) {
        return RequestResult{};
    }

    const std::string_view line = *line_res.line;

    // Chunk-size lines are body framing, not head: the body cap bounds those, and charging them here rejects an upload
    // sent in small chunks.
    if (phase_ != Phase::ChunkSize) {
        head_bytes_ += line.size() + crlf_size;
        if (head_bytes_ > max_head_bytes) {
            return fail(RequestError::HeadTooLarge);
        }
    }

    switch (phase_) {
        case Phase::RequestLine:
            return handle_request_line(line);
        case Phase::Headers:
            return handle_field_line(line);
        case Phase::ChunkSize:
            return handle_chunk_size(line);
        case Phase::Trailers:
            return handle_trailer_line(line);
        case Phase::Body:
        case Phase::Discard:
        case Phase::ChunkData:
            break;
    }
    return std::nullopt;
}

RequestResult RequestReader::next_request() {
    if (failure_ != RequestError::None) {
        return fail(failure_);
    }

    // Seven phases, two ways of consuming input, and no default: a new phase has to say which it is.
    while (true) {
        std::optional<RequestResult> result;
        switch (phase_) {
            case Phase::Body:
            case Phase::Discard:
            case Phase::ChunkData:
                result = read_body_bytes();
                break;
            case Phase::RequestLine:
            case Phase::Headers:
            case Phase::ChunkSize:
            case Phase::Trailers:
                result = read_next_line();
                break;
        }

        if (result.has_value()) {
            return std::move(*result);
        }
    }
}

}  // namespace carafe::http
