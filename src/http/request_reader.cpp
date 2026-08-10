#include "http/request_reader.hpp"

#include <carafe/http/request.hpp>

#include "http/line_reader.hpp"
#include "http/request_parser.hpp"

#include <cstddef>
#include <optional>
#include <string_view>
#include <utility>

namespace carafe::http {

// Request line plus every field line. The per-line cap bounds one field; this
// bounds how many a client may stack up.
constexpr std::size_t max_head_bytes = 65536;

// Bytes alone would admit thousands of tiny fields, whose vector and string
// overhead costs far more than their bytes suggest.
constexpr std::size_t max_fields = 100;

// Stripped by LineReader, but still bytes the client sent.
constexpr std::size_t crlf_size = 2;

RequestResult RequestReader::fail(RequestError error) {
    failure_ = error;
    return {error, std::nullopt};
}

void RequestReader::append(std::string_view bytes) {
    line_reader_.append(bytes);
}

std::optional<RequestResult> RequestReader::handle_request_line(std::string_view line) {
    RequestLineResult parsed = parse_request_line(line);

    // No default: a new ParseError member must break this build rather than
    // fall through to a generic 400.
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

    // Counted before parsing, so a flood of fields costs the attacker more than
    // it costs us.
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

RequestResult RequestReader::complete_head() {
    // RFC 9112 §3.2 requires exactly one Host on 1.1. Two are ambiguous at any
    // version, and ambiguity about the target host is a routing decision an
    // attacker would be making.
    const auto host_count = request_.headers.count("host");
    if (host_count > 1 || (host_count == 0 && request_.version == Version::Http11)) {
        return fail(RequestError::Malformed);
    }

    Request ready = std::move(request_);

    // Only success resets. After a failure the stream position is unknown, so
    // there is nothing safe to resume from.
    phase_ = Phase::RequestLine;
    request_ = Request{};
    head_bytes_ = 0;
    field_count_ = 0;

    return {RequestError::None, std::move(ready)};
}

RequestResult RequestReader::next_request() {
    if (failure_ != RequestError::None) {
        return {failure_, std::nullopt};
    }

    while (true) {
        const LineResult line_res = line_reader_.next_line();

        // One LineError, two answers: only this layer knows which line it asked
        // for, which is why the ambiguity ends here.
        if (line_res.error == LineError::LineTooLong) {
            return fail(phase_ == Phase::RequestLine ? RequestError::RequestLineTooLong
                                                     : RequestError::HeaderTooLong);
        }

        if (!line_res.line.has_value()) {
            return {};
        }

        const std::string_view line = *line_res.line;
        head_bytes_ += line.size() + crlf_size;
        if (head_bytes_ > max_head_bytes) {
            return fail(RequestError::HeadTooLarge);
        }

        auto result =
            phase_ == Phase::RequestLine ? handle_request_line(line) : handle_field_line(line);
        if (result.has_value()) {
            return std::move(*result);
        }
    }
}

}  // namespace carafe::http
