#include "http/request_reader.hpp"

#include <carafe/http/request.hpp>

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

// What lets the digit loop carry one bound instead of two: the cap is tested after each digit, so the next multiply
// starts below it and cannot wrap.
static_assert(max_drain_bytes <= (std::numeric_limits<std::size_t>::max() - 9) / 10);

namespace {

struct BodyLength {
    RequestError error = RequestError::None;
    std::size_t bytes = 0;
};

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

// RFC 9112 §6.3, minus the rules only a response can reach.
[[nodiscard]] BodyLength measure_body(const Headers& headers) noexcept {
    // §6.1: a coding we do not implement leaves us unable to say where the body ends, so there is nothing to read past
    // and nothing to resume from.
    if (headers.contains("transfer-encoding")) {
        return {RequestError::UnsupportedTransferEncoding, 0};
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

    const BodyLength body = measure_body(request_.headers);
    if (body.error != RequestError::None) {
        return fail(body.error);
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

RequestResult RequestReader::next_request() {
    if (failure_ != RequestError::None) {
        return fail(failure_);
    }

    while (true) {
        if (phase_ == Phase::Discard) {
            body_bytes_ -= line_reader_.discard(body_bytes_);
            if (body_bytes_ > 0) {
                return {};
            }
            arm_next_request();
            continue;
        }

        if (phase_ == Phase::Body) {
            const auto body = line_reader_.take(body_bytes_);
            if (!body.has_value()) {
                return {};
            }
            request_.body.assign(*body);
            return finish_request();
        }

        const LineResult line_res = line_reader_.next_line();

        // One LineError, two answers: only this layer knows which line it asked for.
        if (line_res.error == LineError::LineTooLong) {
            return fail(phase_ == Phase::RequestLine ? RequestError::RequestLineTooLong : RequestError::HeaderTooLong);
        }

        if (!line_res.line.has_value()) {
            return {};
        }

        const std::string_view line = *line_res.line;
        head_bytes_ += line.size() + crlf_size;
        if (head_bytes_ > max_head_bytes) {
            return fail(RequestError::HeadTooLarge);
        }

        auto result = phase_ == Phase::RequestLine ? handle_request_line(line) : handle_field_line(line);
        if (result.has_value()) {
            return std::move(*result);
        }
    }
}

}  // namespace carafe::http
