#pragma once

#include <cstddef>
#include <optional>
#include <string_view>

namespace carafe::net {

// A view into the caller's buffer, or nothing. Nothing means the peer closed:
// on a blocking socket read() returns 0 only at end of stream.
struct ReadResult {
    int os_error = 0;
    std::optional<std::string_view> bytes;

    [[nodiscard]] explicit operator bool() const noexcept {
        return os_error == 0;
    }
};

// Every byte or none of them: a short write is an obligation, not a result, so
// this does not hand one back.
struct WriteResult {
    int os_error = 0;

    [[nodiscard]] explicit operator bool() const noexcept {
        return os_error == 0;
    }
};

// Sole owner of one file descriptor, closed exactly once when the Socket dies.
// An empty Socket holds -1 and owns nothing.
class Socket {
public:
    // -1 is accepted: a failed accept() has no descriptor to give.
    explicit Socket(int fd) noexcept : fd_(fd) {}

    ~Socket();

    // Prevent dual ownership of the socket fd and use-after-close
    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;

    // Move semantics to transfer ownership of the socket fd
    Socket(Socket&&) noexcept;
    Socket& operator=(Socket&&) noexcept;

    // Accessor for the underlying file descriptor
    [[nodiscard]] int get() const noexcept {
        return fd_;
    }

    [[nodiscard]] bool valid() const noexcept {
        return fd_ != -1;
    }

    // Up to size bytes into the caller's buffer. The view in the result points
    // into that buffer and dies with it.
    [[nodiscard]] ReadResult read(char* buffer, std::size_t size);

    // All bytes from the caller's buffer. The caller must ensure that the buffer
    // remains valid until the write completes.
    [[nodiscard]] WriteResult write(std::string_view bytes);

private:
    // The file descriptor owned by this Socket. -1 means invalid socket.
    int fd_ = -1;
};

}  // namespace carafe::net
