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

// Every byte or a failure, with no count either way. A short write is an obligation while it can be met; where it
// cannot, a send deadline having fired part-way, the connection is finished and a count says nothing worth acting
// on.
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

    // Every byte or a failure: EINTR is retried, so a short write never surfaces on its own. A send deadline can
    // stop one part-way, which is the only case where some bytes went and the result says failure.
    [[nodiscard]] WriteResult write(std::string_view bytes);

    // A deadline on each recv, after which read() reports EAGAIN rather than waiting on. Zero on success, otherwise the
    // errno the socket refused with: a caller that cannot bound its reads needs the reason, not only the fact.
    [[nodiscard]] int set_receive_timeout(std::chrono::milliseconds limit) noexcept;

    // The same for send, so a peer that stops reading cannot hold a thread in write().
    [[nodiscard]] int set_send_timeout(std::chrono::milliseconds limit) noexcept;

private:
    int fd_ = -1;
};

}  // namespace carafe::net
