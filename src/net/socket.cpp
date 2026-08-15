#include "net/socket.hpp"

#include <cerrno>
#include <cstddef>
#include <optional>
#include <string_view>
#include <unistd.h>
#include <utility>

#include <sys/socket.h>

namespace carafe::net {

// Linux releases the descriptor even when close() reports EINTR, so retrying
// would close whatever another thread has since opened into the slot.
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

// Not const: a const Socket& reads as safe to share, and these bytes are gone from
// the stream once taken. std::istream::read is non-const for the same reason.
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

        // A signal arriving mid-wait is not an outcome, just an interruption: ask again.
        if (errno != EINTR) {
            return {errno, std::nullopt};
        }
    }
}

}  // namespace carafe::net
