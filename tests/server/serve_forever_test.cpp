#include <carafe/http/handler.hpp>
#include <carafe/http/request.hpp>
#include <carafe/http/response.hpp>

#include "net/listener.hpp"
#include "net/socket.hpp"
#include "server/router.hpp"
#include "server/serve.hpp"

#include <array>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <arpa/inet.h>
#include <gtest/gtest.h>
#include <sys/socket.h>
#include <sys/time.h>

namespace {

using carafe::http::Method;
using carafe::http::Request;
using carafe::http::text_response;
using carafe::net::listen_on;
using carafe::net::Socket;
using carafe::server::Router;
using carafe::server::serve_forever;

// Loopback rather than a socketpair: serve_forever's whole job is the accept loop, so it needs something to accept.
Socket connect_to(std::uint16_t port) {
    Socket client{::socket(AF_INET, SOCK_STREAM, 0)};
    EXPECT_TRUE(client.valid());

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    const void* addr_ptr = &addr;
    EXPECT_EQ(::connect(client.get(), static_cast<const sockaddr*>(addr_ptr), sizeof(addr)), 0);

    // A deadline everywhere, so a server that never answers fails the test rather than wedging the suite.
    const timeval deadline{5, 0};
    EXPECT_EQ(::setsockopt(client.get(), SOL_SOCKET, SO_RCVTIMEO, &deadline, sizeof(deadline)), 0);
    return client;
}

void send_all(const Socket& sock, std::string_view bytes) {
    while (!bytes.empty()) {
        const ssize_t sent = ::send(sock.get(), bytes.data(), bytes.size(), MSG_NOSIGNAL);
        ASSERT_NE(sent, -1);
        bytes.remove_prefix(static_cast<std::size_t>(sent));
    }
}

// The status line alone, which is all these tests ask of a response. Empty when nothing arrived before the deadline.
std::string status_line(const Socket& sock) {
    std::array<char, 4096> buf{};
    const ssize_t got = ::recv(sock.get(), buf.data(), buf.size(), 0);
    if (got <= 0) {
        return {};
    }
    const std::string_view response(buf.data(), static_cast<std::size_t>(got));
    return std::string{response.substr(0, response.find("\r\n"))};
}

// Starts serve_forever on a kernel-chosen port and reports the port.
//
// The thread is detached, and deliberately: serve_forever returns only when accepting fails, and stopping it from out
// here would mean giving Listener a close() whose only caller is this file. A thread parked in accept() costs nothing
// at exit. The listener moves into the thread rather than being leaked beside it, so nothing out here outlives what
// the thread is still using.
std::uint16_t start_server(std::shared_ptr<Router> router) {
    auto result = listen_on(0);
    EXPECT_TRUE(result.listener.has_value());
    const std::uint16_t port = result.listener->port();

    std::thread([listener = std::move(*result.listener), router = std::move(router)]() mutable {
        serve_forever(listener, router);
    }).detach();

    return port;
}

carafe::http::Handler echo() {
    return [](const Request& request) { return text_response(200, "you asked for " + request.target + "\n"); };
}

// Blocks each handler until `needed` of them are running at once. Served one at a time, the first would wait alone
// until the deadline, so every arrival being released is the proof that they overlapped.
class Rendezvous {
public:
    explicit Rendezvous(std::size_t needed) noexcept : needed_(needed) {}

    bool arrive() {
        std::unique_lock<std::mutex> lock(mutex_);
        ++arrived_;
        if (arrived_ >= needed_) {
            ready_.notify_all();
            return true;
        }
        return ready_.wait_for(lock, std::chrono::seconds(5), [this] { return arrived_ >= needed_; });
    }

private:
    std::mutex mutex_;
    std::condition_variable ready_;
    std::size_t needed_;
    std::size_t arrived_ = 0;
};

// The outage this milestone closes: one client holding a persistent connection open, having asked for nothing more,
// used to leave every other client waiting for as long as it cared to hold on.
TEST(ServeForever, AnswersASecondClientWhileTheFirstHoldsItsConnection) {
    auto router = std::make_shared<Router>();
    router->add(Method::Get, "/hello", echo());
    const std::uint16_t port = start_server(router);

    const Socket holder = connect_to(port);
    send_all(holder, "GET /hello HTTP/1.1\r\nHost: a.test\r\n\r\n");
    ASSERT_EQ(status_line(holder), "HTTP/1.1 200 OK");

    // holder is answered and still open, which is correct HTTP/1.1 and used to be the end of the story.
    const Socket second = connect_to(port);
    send_all(second, "GET /hello HTTP/1.1\r\nHost: b.test\r\n\r\n");
    EXPECT_EQ(status_line(second), "HTTP/1.1 200 OK");
}

// Not a timing measurement: eight handlers have to be inside the server at the same moment for any of them to return,
// which one thread cannot arrange however long it is given.
TEST(ServeForever, RunsHandlersConcurrently) {
    constexpr std::size_t clients = 8;
    auto rendezvous = std::make_shared<Rendezvous>(clients);

    auto router = std::make_shared<Router>();
    // Captured by value, since the server thread outlives this test and the router with it.
    router->add(Method::Get, "/wait",
                [rendezvous](const Request&) { return text_response(rendezvous->arrive() ? 200 : 503, ""); });
    const std::uint16_t port = start_server(router);

    std::vector<std::thread> callers;
    std::vector<std::string> lines(clients);
    callers.reserve(clients);
    for (std::size_t i = 0; i < clients; ++i) {
        callers.emplace_back([port, i, &lines] {
            const Socket client = connect_to(port);
            send_all(client, "GET /wait HTTP/1.1\r\nHost: x.test\r\n\r\n");
            lines[i] = status_line(client);
        });
    }
    for (std::thread& caller : callers) {
        caller.join();
    }

    for (const std::string& line : lines) {
        EXPECT_EQ(line, "HTTP/1.1 200 OK");
    }
}

// One RequestReader per connection, not one per server: two half-sent heads interleaved must not splice into each
// other, which is the hazard a shared parser would introduce the moment connections overlap.
TEST(ServeForever, KeepsEachConnectionsParserSeparate) {
    auto router = std::make_shared<Router>();
    router->add(Method::Get, "/one", echo());
    router->add(Method::Get, "/two", echo());
    const std::uint16_t port = start_server(router);

    const Socket first = connect_to(port);
    const Socket second = connect_to(port);

    send_all(first, "GET /one HTTP/1.1\r\n");
    send_all(second, "GET /two HTTP/1.1\r\n");
    send_all(first, "Host: a.test\r\n\r\n");
    send_all(second, "Host: b.test\r\n\r\n");

    EXPECT_EQ(status_line(first), "HTTP/1.1 200 OK");
    EXPECT_EQ(status_line(second), "HTTP/1.1 200 OK");
}

}  // namespace
