#pragma once

namespace carafe::net {

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

private:
    // The file descriptor owned by this Socket. -1 means invalid socket.
    int fd_ = -1;
};

}  // namespace carafe::net
