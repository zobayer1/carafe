#include "net/socket.hpp"

#include <cerrno>
#include <chrono>
#include <cstddef>
#include <ctime>
#include <optional>
#include <string_view>
#include <unistd.h>
#include <utility>

#include <sys/socket.h>
#include <sys/time.h>

namespace carafe::net {

namespace {

// SO_RCVTIMEO and SO_SNDTIMEO take the same timeval and mean the same thing in opposite directions. A zero timeval
// means no deadline at all rather than an immediate one, which is why a caller computing a remainder clamps first.
[[nodiscard]] int set_timeout(int fd, int option, std::chrono::milliseconds limit) noexcept {
    const auto whole = std::chrono::duration_cast<std::chrono::seconds>(limit);
    timeval deadline{};
    deadline.tv_sec = static_cast<time_t>(whole.count());
    deadline.tv_usec = static_cast<suseconds_t>((limit - whole) / std::chrono::microseconds(1));

    return ::setsockopt(fd, SOL_SOCKET, option, &deadline, sizeof(deadline)) == 0 ? 0 : errno;
}

}  // namespace

std::optional<std::chrono::milliseconds> milliseconds_until(std::chrono::steady_clock::time_point deadline) {
    const auto left = deadline - std::chrono::steady_clock::now();
    if (left < std::chrono::milliseconds(1)) {
        return std::nullopt;
    }
    return std::chrono::duration_cast<std::chrono::milliseconds>(left);
}

// Linux releases the descriptor even when close() reports EINTR, so retrying would close whatever another thread has
// since opened into the slot.
Socket::~Socket() {
    if (fd_ != -1) {
        ::close(fd_);
    }
}

Socket::Socket(Socket&& other) noexcept : fd_(std::exchange(other.fd_, -1)) {}

Socket& Socket::operator=(Socket&& other) noexcept {
    // Guarded: self-assignment would close the descriptor, then take it back.
    if (this != &other) {
        if (fd_ != -1) {
            ::close(fd_);
        }
        fd_ = std::exchange(other.fd_, -1);
    }
    return *this;
}

// Not const: a const Socket& reads as safe to share, and these bytes leave the stream once taken. std::istream::read is
// non-const for the same reason.
// NOLINTNEXTLINE(readability-make-member-function-const)
ReadResult Socket::read(char* buffer, std::size_t size) {
    while (true) {
        const ssize_t bytes_read = ::recv(fd_, buffer, size, 0);
        if (bytes_read > 0) {
            return {0, std::string_view(buffer, static_cast<std::size_t>(bytes_read))};
        }

        // Zero only ever means end of stream here, since the socket blocks.
        if (bytes_read == 0) {
            return {0, std::nullopt};
        }

        // EINTR is an interruption, not an outcome: ask again.
        if (errno != EINTR) {
            return {errno, std::nullopt};
        }
    }
}

// NOLINTNEXTLINE(readability-make-member-function-const)
WriteResult Socket::write(std::string_view bytes, std::chrono::milliseconds limit) {
    const auto deadline = std::chrono::steady_clock::now() + limit;

    while (!bytes.empty()) {
        const auto left = milliseconds_until(deadline);
        if (!left) {
            return {ETIMEDOUT};
        }
        if (const int refused = set_timeout(fd_, SO_SNDTIMEO, *left); refused != 0) {
            return {refused};
        }

        const ssize_t written = ::send(fd_, bytes.data(), bytes.size(), MSG_NOSIGNAL);
        if (written == -1) {
            // EINTR is an interruption, not an outcome: ask again.
            if (errno != EINTR) {
                return {errno};
            }
            continue;
        }
        bytes.remove_prefix(static_cast<std::size_t>(written));
    }
    return {0};
}

// Not const in a plainer sense than read and write: this changes how the socket behaves from here on.
// NOLINTNEXTLINE(readability-make-member-function-const)
int Socket::set_receive_timeout(std::chrono::milliseconds limit) noexcept {
    return set_timeout(fd_, SO_RCVTIMEO, limit);
}

}  // namespace carafe::net
