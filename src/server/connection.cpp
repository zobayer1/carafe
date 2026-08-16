#include "server/connection.hpp"

#include "net/socket.hpp"

#include <optional>
#include <utility>

namespace carafe::server {

Connection::Connection(net::Socket socket) : socket_(std::move(socket)) {}

ConnectionResult Connection::next_request() {
    while (true) {
        // Parsed before read, not after: the previous read may already have carried
        // this request, and a pipelining client will not send more until it is
        // answered. Reading first would wait for bytes that have already arrived.
        auto parsed = reader_.next_request();
        if (!parsed) {
            return {parsed.error, 0, std::nullopt};
        }
        if (parsed.request) {
            return {http::RequestError::None, 0, std::move(parsed.request)};
        }

        const auto chunk = socket_.read(buffer_.data(), buffer_.size());
        if (!chunk) {
            return {http::RequestError::None, chunk.os_error, std::nullopt};
        }

        // A close part-way through a head is reported as a finished connection
        // rather than a bad request: the peer that hung up cannot be told either way.
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
