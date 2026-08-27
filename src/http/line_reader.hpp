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

// A buffer over a byte stream, reassembled across chunk boundaries: it yields
// CRLF-terminated lines, hands over a byte count, or drops one.
class LineReader {
public:
    // Invalidates every view handed out by next_line() and take(): the buffer
    // reallocates and compacts here.
    void append(std::string_view bytes);

    // The next complete line without its CRLF, as a view into the internal
    // buffer, valid only until the next append().
    [[nodiscard]] LineResult next_line();

    // The next `n` bytes unchanged, viewed like next_line's; nothing yet if fewer
    // have arrived. No cap, unlike next_line(): a body's length is known from its
    // Content-Length, so the caller refuses an oversized one before asking.
    [[nodiscard]] std::optional<std::string_view> take(std::size_t n);

    // Drops up to `n` buffered bytes and reports how many, fewer than asked while
    // the rest is in flight. Not take(): waiting for a whole body before dropping
    // it would buffer exactly the bytes we refused.
    [[nodiscard]] std::size_t discard(std::size_t n) noexcept;

private:
    std::string buffer_;
    std::size_t begin_ = 0;

    // No terminator lies before this. Bytes handed out or dropped were never
    // scanned, so both reset it to begin_ rather than leave it behind: a search
    // resuming behind them would find a CRLF inside, and compaction would wrap.
    std::size_t scanned_ = 0;
};

}  // namespace carafe::http
