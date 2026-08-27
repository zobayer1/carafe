#pragma once

#include <carafe/http/request.hpp>

#include "http/request_reader.hpp"
#include "net/socket.hpp"

#include <array>
#include <optional>
#include <string_view>

namespace carafe::server {

// Two failures, not one, because they call for opposite actions: a malformed head
// gets a status written back, a failed read means nobody is left to write to. An
// empty request with neither set is the client having finished.
struct ConnectionResult {
    http::RequestError error = http::RequestError::None;
    int os_error = 0;
    std::optional<http::Request> request;

    // From the reader: false when the byte stream cannot be resynchronised, so the
    // caller answers this failure and closes rather than looking for another request.
    bool stream_continues = true;

    [[nodiscard]] explicit operator bool() const noexcept {
        return error == http::RequestError::None && os_error == 0;
    }
};

// A socket and the parser state for the bytes coming off it. Members rather than
// parameters: a half-received head lives in the reader between reads, and sharing
// one would splice two clients into one request.
class Connection {
public:
    explicit Connection(net::Socket socket);

    [[nodiscard]] ConnectionResult next_request();

    // Straight to the socket: a response touches no parser state, so this cannot
    // disturb a partially received next request sitting in the reader.
    [[nodiscard]] net::WriteResult write(std::string_view bytes);

private:
    net::Socket socket_;
    http::RequestReader reader_;
    std::array<char, 4096> buffer_{};
};

}  // namespace carafe::server
