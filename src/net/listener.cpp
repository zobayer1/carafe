#include "net/listener.hpp"

#include "net/socket.hpp"

#include <cerrno>
#include <cstdint>
#include <utility>

#include <arpa/inet.h>
#include <sys/socket.h>

namespace carafe::net {

AcceptResult Listener::accept() {
    while (true) {
        // accept4, not accept: flags are not inherited, so a plain accept() would hand
        // back a client without CLOEXEC however the listener was made.
        const int client_fd = ::accept4(socket_.get(), nullptr, nullptr, SOCK_CLOEXEC);

        if (client_fd != -1) {
            return {0, Socket(client_fd)};
        }

        // A signal arriving mid-wait is not an outcome, just an interruption: ask again.
        if (errno != EINTR) {
            return {errno, std::nullopt};
        }
    }
}

ListenResult listen_on(std::uint16_t port) {
    // CLOEXEC in the type argument, not fcntl afterwards: nothing a concurrent
    // fork can copy in between.
    const int raw_fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (raw_fd == -1) {
        return {ListenError::SocketFailed, errno, std::nullopt};
    }

    // Owned from here on, so every return below closes it -- and each reads errno
    // into the result first, before that close can overwrite it.
    Socket sock(raw_fd);

    // Ignored: losing SO_REUSEADDR costs a slow restart, not correctness.
    const int opt = 1;
    ::setsockopt(sock.get(), SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    // Zeroed: a stale sin_zero fails the bind with nothing in the code to show why.
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    // Via void*: sockaddr is a C tagged union, and casting to it is how the API is called.
    const void* bind_ptr = &addr;
    if (::bind(sock.get(), static_cast<const sockaddr*>(bind_ptr), sizeof(addr)) == -1) {
        return {ListenError::BindFailed, errno, std::nullopt};
    }

    // Port 0 asked the kernel to choose, and this is the only way to learn which.
    std::uint16_t actual_port = port;
    if (port == 0) {
        sockaddr_in bound_addr{};
        socklen_t addr_len = sizeof(bound_addr);

        void* get_ptr = &bound_addr;
        if (::getsockname(sock.get(), static_cast<sockaddr*>(get_ptr), &addr_len) == -1) {
            return {ListenError::AddressFailed, errno, std::nullopt};
        }
        actual_port = ntohs(bound_addr.sin_port);
    }

    if (::listen(sock.get(), SOMAXCONN) == -1) {
        return {ListenError::ListenFailed, errno, std::nullopt};
    }

    return {ListenError::None, 0, Listener{std::move(sock), actual_port}};
}

}  // namespace carafe::net
