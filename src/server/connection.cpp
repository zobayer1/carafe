#include "server/connection.hpp"

#include "net/socket.hpp"

#include <cerrno>
#include <chrono>
#include <optional>
#include <utility>

namespace carafe::server {

Connection::Connection(net::Socket socket, Deadlines deadlines) : socket_(std::move(socket)), deadlines_(deadlines) {}

ConnectionResult Connection::next_request() {
    while (true) {
        // Parsed before read: the previous read may already have carried this request, and a pipelining client sends no
        // more until it is answered.
        auto parsed = reader_.next_request();

        // Anything but "nothing yet": a failure carries no request, a success carries one, and both carry whatever the
        // reader said about the stream.
        if (!parsed || parsed.request) {
            if (parsed.request.has_value()) {
                request_in_progress_ = false;
            }
            return {parsed.error, 0, std::move(parsed.request), parsed.stream_continues, parsed.version};
        }

        const int deadline_error = apply_read_deadline();
        if (deadline_error != 0) {
            return {http::RequestError::None, deadline_error, std::nullopt, false};
        }

        const auto chunk = socket_.read(buffer_.data(), buffer_.size());
        if (!chunk) {
            return {http::RequestError::None, chunk.os_error, std::nullopt, false};
        }

        // A close part-way through a head is a finished connection, not a bad request: the peer that hung up cannot be
        // told either way.
        if (!chunk.bytes) {
            return {};
        }
        reader_.append(*chunk.bytes);
        if (!request_in_progress_) {
            request_in_progress_ = true;
            request_deadline_ = std::chrono::steady_clock::now() + deadlines_.request;
        }
    }
}

net::WriteResult Connection::write(std::string_view bytes) {
    // The same patience as receiving, since it is the same client either way. A third deadline is the honest split
    // the moment they should differ.
    if (const int refused = socket_.set_send_timeout(deadlines_.request); refused != 0) {
        return {refused};
    }
    return socket_.write(bytes);
}

int Connection::apply_read_deadline() {
    auto limit = deadlines_.idle;

    if (request_in_progress_) {
        const auto left = request_deadline_ - std::chrono::steady_clock::now();

        // Under a millisecond is no time left at all. Rounding it down instead would hand SO_RCVTIMEO a zero timeval,
        // which means no deadline rather than an immediate one, and turn the last instant of one into a wait forever.
        if (left < std::chrono::milliseconds(1)) {
            return ETIMEDOUT;
        }

        limit = std::chrono::duration_cast<std::chrono::milliseconds>(left);
    }

    return socket_.set_receive_timeout(limit);
}

}  // namespace carafe::server
