#include "server/connection.hpp"

#include "net/socket.hpp"

#include <optional>
#include <utility>

namespace carafe::server {

Connection::Connection(net::Socket socket) : socket_(std::move(socket)) {}

ConnectionResult Connection::next_request() {
    while (true) {
        // Parsed before read: the previous read may already have carried this
        // request, and a pipelining client sends no more until it is answered.
        auto parsed = reader_.next_request();

        // Anything but "nothing yet": a failure carries no request, a success
        // carries one, and both carry whatever the reader said about the stream.
        if (!parsed || parsed.request) {
            return {parsed.error, 0, std::move(parsed.request), parsed.stream_continues,
                    parsed.version};
        }

        const auto chunk = socket_.read(buffer_.data(), buffer_.size());
        if (!chunk) {
            return {http::RequestError::None, chunk.os_error, std::nullopt, false};
        }

        // A close part-way through a head is a finished connection, not a bad
        // request: the peer that hung up cannot be told either way.
        if (!chunk.bytes) {
            return {};
        }
        reader_.append(*chunk.bytes);
    }
}

net::WriteResult Connection::write(std::string_view bytes) {
    return socket_.write(bytes);
}

}  // namespace carafe::server
