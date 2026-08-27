#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace carafe::http {

// Not one status each, unlike ParseError: an overlong line is 414 for a request
// line and 431 for a header.
enum class LineError {
    None,
    LineTooLong,
};

// Three states, not two: a line, nothing yet, or a failure. An engaged but empty
// `line` is the blank line ending a header block.
struct LineResult {
    LineError error = LineError::None;
    std::optional<std::string_view> line;

    [[nodiscard]] explicit operator bool() const noexcept {
        return error == LineError::None;
    }
};

// Splits a byte stream into CRLF-terminated lines across chunk boundaries.
class LineReader {
public:
    // Invalidates every view handed out by next_line() and take(): the buffer
    // reallocates and compacts here.
    void append(std::string_view bytes);

    // The next complete line without its CRLF, as a view into the internal
    // buffer, valid only until the next append().
    [[nodiscard]] LineResult next_line();

    // The next `n` bytes unchanged, as a view into the internal buffer, valid
    // only until the next append(); nothing yet if fewer have arrived. No cap,
    // unlike next_line(): a body's length is known from its Content-Length
    // before the first byte is taken, so an oversized one is refused by the
    // caller rather than discovered halfway through.
    [[nodiscard]] std::optional<std::string_view> take(std::size_t n);

private:
    std::string buffer_;
    std::size_t begin_ = 0;
    std::size_t scanned_ = 0;
};

}  // namespace carafe::http
