#include "http/line_reader.hpp"

#include <algorithm>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace carafe::http {

// RFC 9112 §3 asks for at least 8000 octets; rounded up.
constexpr std::size_t max_line_length = 8192;

// A bare LF ends nothing, so a proxy requiring CRLF cannot disagree with us.
constexpr std::string_view crlf{"\r\n"};

void LineReader::append(std::string_view bytes) {
    // Only once the dead prefix outweighs what is left, so no compaction moves more bytes than it reclaims. Both
    // indices are offsets, so both shift.
    if (begin_ > 0 && (begin_ * 2 >= buffer_.size())) {
        buffer_.erase(0, begin_);
        scanned_ -= begin_;
        begin_ = 0;
    }
    buffer_.append(bytes.data(), bytes.size());
}

// Resuming from scanned_ rather than begin_ keeps a dribbled line linear.
LineResult LineReader::next_line() {
    auto crlf_pos = buffer_.find(crlf, scanned_);

    // No terminator yet. The cap still applies, or a client dribbles forever.
    if (crlf_pos == std::string::npos) {
        if (buffer_.size() - begin_ > max_line_length) {
            return LineResult{LineError::LineTooLong, {}};
        }

        // Resume behind a possible half-terminator, by addition so it cannot wrap.
        constexpr std::size_t lookback = crlf.size() - 1;
        if (buffer_.size() < begin_ + lookback) {
            scanned_ = begin_;
        } else {
            scanned_ = buffer_.size() - lookback;
        }

        return LineResult{LineError::None, std::nullopt};
    }

    // Checked here too, or one large append walks past a cap guarding only the search failure.
    if (crlf_pos - begin_ > max_line_length) {
        return LineResult{LineError::LineTooLong, {}};
    }

    auto line = std::string_view(buffer_.data() + begin_, crlf_pos - begin_);
    begin_ = crlf_pos + crlf.size();
    scanned_ = begin_;

    return LineResult{LineError::None, line};
}

std::optional<std::string_view> LineReader::take(std::size_t n) {
    if (buffer_.size() - begin_ < n) {
        return std::nullopt;
    }

    auto bytes = std::string_view(buffer_.data() + begin_, n);
    begin_ += n;
    scanned_ = begin_;

    return bytes;
}

std::size_t LineReader::discard(std::size_t n) noexcept {
    const std::size_t dropped = std::min(n, buffer_.size() - begin_);
    begin_ += dropped;
    scanned_ = begin_;
    return dropped;
}

}  // namespace carafe::http
