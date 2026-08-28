#pragma once

#include <carafe/http/request.hpp>

#include "http/line_reader.hpp"

#include <cstddef>
#include <optional>
#include <string_view>

namespace carafe::http {

// Each value maps to one status, but the mapping is many to one.
enum class RequestError {
    None,
    Malformed,                    // 400
    UnknownMethod,                // 501
    UnsupportedVersion,           // 505
    RequestLineTooLong,           // 414
    HeaderTooLong,                // 431
    TooManyHeaders,               // 431
    HeadTooLarge,                 // 431
    BodyTooLarge,                 // 413
    UnsupportedTransferEncoding,  // 501
};

// Three states, like LineResult: nothing yet, a request, or a failure.
struct RequestResult {
    RequestError error = RequestError::None;
    std::optional<Request> request;

    // False when the reader cannot find where the next request begins, which is
    // every failure but a body it is willing to read and drop.
    bool stream_continues = true;

    // The version the failed request declared. RFC 9112 §9.3 leaves an HTTP/1.0
    // client waiting for a close, and a failure carries no Request to read it from.
    Version version{};

    [[nodiscard]] explicit operator bool() const noexcept {
        return error == RequestError::None;
    }
};

// Turns a byte stream into successive request heads. One per connection, not
// one per request: completion arms the next, so a pipelined stream just works.
class RequestReader {
public:
    void append(std::string_view bytes);

    // Drains every line available, so a caller does not loop. Unlike
    // LineReader, nothing here is a view: a Request owns all of its strings.
    [[nodiscard]] RequestResult next_request();

private:
    enum class Phase { RequestLine, Headers, Body, Discard };

    // Loses the stream: the failure latches and every later call repeats it.
    [[nodiscard]] RequestResult fail(RequestError error);

    // Answerable and read past: the failure is reported once, the declared body is
    // dropped, and the next request is parsed as usual.
    [[nodiscard]] RequestResult refuse(RequestError error);

    // Clears the per-request state, and so is called wherever the next request's
    // first byte is known: a completed request, or a refused body fully drained.
    void arm_next_request();

    // Hands the assembled request over and arms the next one.
    [[nodiscard]] RequestResult finish_request();

    // Engaged means "hand this to the caller"; empty means "read another line".
    [[nodiscard]] std::optional<RequestResult> handle_request_line(std::string_view line);
    [[nodiscard]] std::optional<RequestResult> handle_field_line(std::string_view line);

    // Validates the assembled head and decides what follows it: a request to hand
    // over, a failure, or a body still to be read.
    [[nodiscard]] std::optional<RequestResult> complete_head();

    Phase phase_ = Phase::RequestLine;
    LineReader line_reader_;
    Request request_;
    RequestError failure_ = RequestError::None;
    std::size_t head_bytes_ = 0;
    std::size_t body_bytes_ = 0;
    std::size_t field_count_ = 0;
};

}  // namespace carafe::http
