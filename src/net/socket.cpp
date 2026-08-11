#include "net/socket.hpp"

#include <unistd.h>
#include <utility>

namespace carafe::net {

// Linux releases the descriptor even when close() reports EINTR, so retrying
// would close whatever another thread has since opened into the slot.
Socket::~Socket() {
    if (fd_ != invalid_fd) {
        ::close(fd_);
    }
}

Socket::Socket(Socket&& other) noexcept : fd_(std::exchange(other.fd_, invalid_fd)) {}

Socket& Socket::operator=(Socket&& other) noexcept {
    // Guarded: self-assignment would close the descriptor, then take it back.
    if (this != &other) {
        if (fd_ != invalid_fd) {
            ::close(fd_);
        }
        fd_ = std::exchange(other.fd_, invalid_fd);
    }
    return *this;
}

}  // namespace carafe::net
