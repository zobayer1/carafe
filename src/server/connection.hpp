#pragma once

#include <carafe/http/request.hpp>

#include "http/request_reader.hpp"
#include "net/socket.hpp"

#include <array>
#include <chrono>
#include <optional>
#include <string_view>

namespace carafe::server {

// Two failures, not one, because they call for opposite actions: a malformed head gets a status written back, a failed
// read means nobody is left to write to. An empty request with neither set is the client having finished.
struct ConnectionResult {
    http::RequestError error = http::RequestError::None;
    int os_error = 0;
    std::optional<http::Request> request;

    // From the reader: false when the byte stream cannot be resynchronised, so the caller answers this failure and
    // closes rather than looking for another request.
    bool stream_continues = true;

    // Forwarded from the reader, and meaningful on a failure: a success carries its own version inside the Request.
    http::Version version{};

    [[nodiscard]] explicit operator bool() const noexcept {
        return error == http::RequestError::None && os_error == 0;
    }
};

// How long a connection may wait, in two parts. Separate because waiting for a request to begin should be generous to a
// client with nothing to say yet, and waiting for one to finish should not: the second is the only one a client can
// renew by sending anything at all.
struct Deadlines {
    std::chrono::milliseconds idle{std::chrono::seconds(30)};
    std::chrono::milliseconds request{std::chrono::seconds(30)};
};

// A socket and the parser state for the bytes coming off it. Members rather than parameters: a half-received head lives
// in the reader between reads, and sharing one would splice two clients into one request.
class Connection {
public:
    explicit Connection(net::Socket socket, Deadlines deadlines = {});

    [[nodiscard]] ConnectionResult next_request();

    // Straight to the socket: a response touches no parser state, so this cannot disturb a partially received next
    // request sitting in the reader.
    [[nodiscard]] net::WriteResult write(std::string_view bytes);

    // Bytes arrived that no handed-over request has claimed, so the client asked for something and is owed an answer.
    // False between requests, which is when a deadline firing is just an idle connection.
    [[nodiscard]] bool request_in_progress() const noexcept {
        return request_in_progress_;
    }

private:
    net::Socket socket_;
    http::RequestReader reader_;
    std::array<char, 4096> buffer_{};
    bool request_in_progress_ = false;
    Deadlines deadlines_;

    // When the request now arriving must be finished by. Set by the first byte of a request and not by each read: the
    // per-read deadline it replaces is exactly what a slow drip renews for ever.
    std::chrono::steady_clock::time_point request_deadline_{};

    // The deadline the next read must carry, applied to the socket. Zero on success; otherwise an errno the caller
    // reports as a read failure, because a read that cannot be bounded is one that may never return.
    [[nodiscard]] int apply_read_deadline();
};

}  // namespace carafe::server
