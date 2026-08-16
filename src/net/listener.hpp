#pragma once

#include "net/socket.hpp"

#include <cstdint>
#include <optional>
#include <utility>

namespace carafe::net {

// One failure channel, so os_error alone answers whether this worked -- the same
// test ReadResult and WriteResult use. `client` is engaged exactly when os_error
// is zero, but saying so twice would invite the two to disagree.
struct AcceptResult {
    int os_error = 0;
    std::optional<Socket> client;

    [[nodiscard]] explicit operator bool() const noexcept {
        return os_error == 0;
    }
};

// A socket already listening, and the port it is bound to. The port is a member
// because binding port 0 lets the kernel choose, and nothing else records the choice.
class Listener {
public:
    Listener(Socket sock, std::uint16_t port) noexcept : socket_(std::move(sock)), port_(port) {}

    [[nodiscard]] std::uint16_t port() const noexcept {
        return port_;
    }

    [[nodiscard]] AcceptResult accept();

private:
    Socket socket_;
    std::uint16_t port_ = 0;
};

// One value per syscall that can fail. Which call failed is all this says;
// os_error says why.
enum class ListenError {
    None,
    SocketFailed,
    BindFailed,
    AddressFailed,
    ListenFailed,
};

// Two states, unlike RequestResult: a listener, or the reason there is none.
// os_error is the errno at the failing call, since BindFailed alone cannot tell
// a port already taken from one this user may not have.
struct ListenResult {
    ListenError error = ListenError::None;
    int os_error = 0;
    std::optional<Listener> listener;

    [[nodiscard]] explicit operator bool() const noexcept {
        return error == ListenError::None;
    }
};

// socket -> SO_REUSEADDR -> bind -> listen. Port 0 asks the kernel to choose one,
// which port() then reports.
[[nodiscard]] ListenResult listen_on(std::uint16_t port);

}  // namespace carafe::net
