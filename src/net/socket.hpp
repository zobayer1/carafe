#pragma once

namespace carafe::net {

// Sole owner of one file descriptor, closed exactly once when the Socket dies.
// An empty Socket holds invalid_fd and owns nothing.
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
        return fd_ != invalid_fd;
    }

private:
    static constexpr int invalid_fd = -1;

    int fd_ = invalid_fd;
};

}  // namespace carafe::net
