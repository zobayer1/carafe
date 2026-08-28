#pragma once

#include <chrono>
#include <cstddef>
#include <optional>
#include <string_view>

namespace carafe::net {

// A view into the caller's buffer, or nothing. Nothing means the peer closed: on a blocking socket read() returns 0
// only at end of stream.
struct ReadResult {
    int os_error = 0;
    std::optional<std::string_view> bytes;

    [[nodiscard]] explicit operator bool() const noexcept {
        return os_error == 0;
    }
};

// Every byte or none of them: a short write is an obligation, not a result, so this does not hand one back.
struct WriteResult {
    int os_error = 0;

    [[nodiscard]] explicit operator bool() const noexcept {
        return os_error == 0;
    }
};

// Sole owner of one file descriptor, closed exactly once when the Socket dies. An empty Socket holds -1 and owns
// nothing.
class Socket {
public:
    // -1 is accepted: a failed accept() has no descriptor to give.
    explicit Socket(int fd) noexcept : fd_(fd) {}

    ~Socket();

    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;

    Socket(Socket&&) noexcept;
    Socket& operator=(Socket&&) noexcept;

    [[nodiscard]] int get() const noexcept {
        return fd_;
    }

    [[nodiscard]] bool valid() const noexcept {
        return fd_ != -1;
    }

    // Up to `size` bytes into the caller's buffer. The view in the result points into that buffer and dies with it.
    [[nodiscard]] ReadResult read(char* buffer, std::size_t size);

    // Every byte or a failure: EINTR is retried, so a short write never surfaces.
    [[nodiscard]] WriteResult write(std::string_view bytes);

    // A deadline on each recv, after which read() reports EAGAIN rather than waiting on. False when the socket refused
    // it, which leaves the caller a connection whose reads it cannot bound.
    [[nodiscard]] bool set_receive_timeout(std::chrono::milliseconds limit) noexcept;

private:
    int fd_ = -1;
};

}  // namespace carafe::net
