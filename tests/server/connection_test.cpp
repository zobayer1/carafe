#include "server/connection.hpp"

#include "http/printers.hpp"
#include "net/socket.hpp"

#include <array>
#include <cerrno>
#include <chrono>
#include <string_view>
#include <thread>
#include <utility>

#include <gtest/gtest.h>
#include <sys/socket.h>
#include <sys/time.h>

namespace {

using carafe::http::Method;
using carafe::http::RequestError;
using carafe::http::Version;
using carafe::net::Socket;
using carafe::server::Connection;

// AF_UNIX again: a Connection wants a byte stream with a far end, not a network.
// The deadline is what makes a Connection that waits for bytes it already has
// fail the test instead of wedging the suite.
std::pair<Socket, Socket> connected_pair() {
    std::array<int, 2> fds{-1, -1};
    EXPECT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds.data()), 0);

    const timeval deadline{2, 0};
    for (const int fd : fds) {
        EXPECT_EQ(::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &deadline, sizeof(deadline)), 0);
    }
    return {Socket{fds[0]}, Socket{fds[1]}};
}

// Heads are small enough that one send delivers them whole, so no loop.
void send_all(const Socket& sock, std::string_view bytes) {
    EXPECT_EQ(::send(sock.get(), bytes.data(), bytes.size(), 0),
              static_cast<ssize_t>(bytes.size()));
}

constexpr std::string_view get_root = "GET / HTTP/1.1\r\nHost: example.test\r\n\r\n";

TEST(Connection, ReturnsTheRequestOnceTheHeadIsComplete) {
    auto pair = connected_pair();
    send_all(pair.first, get_root);

    Connection conn{std::move(pair.second)};
    const auto result = conn.next_request();

    ASSERT_TRUE(result);
    EXPECT_EQ(result.error, RequestError::None);
    EXPECT_EQ(result.os_error, 0);
    ASSERT_TRUE(result.request.has_value());
    EXPECT_EQ(result.request->method, Method::Get);
    EXPECT_EQ(result.request->target, "/");
    EXPECT_EQ(result.request->version, Version::Http11);
    ASSERT_TRUE(result.request->headers.get("host").has_value());
    EXPECT_EQ(*result.request->headers.get("host"), "example.test");
}

// The reason the reader is a member: the head arrives across two reads, and the
// half of it seen first has to survive until the rest turns up.
TEST(Connection, AssemblesAHeadSplitAcrossReads) {
    auto pair = connected_pair();
    send_all(pair.first, "GET /split HTTP/1.1\r\n");

    std::thread rest([&pair] {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        send_all(pair.first, "Host: example.test\r\n\r\n");
    });

    Connection conn{std::move(pair.second)};
    const auto result = conn.next_request();
    rest.join();

    ASSERT_TRUE(result);
    ASSERT_TRUE(result.request.has_value());
    EXPECT_EQ(result.request->target, "/split");
}

// Two requests in one segment, then the client hangs up. Reading before parsing
// would find end of stream on the second call and lose a request already in hand.
TEST(Connection, ReturnsASecondRequestAlreadyInTheBuffer) {
    auto pair = connected_pair();
    send_all(pair.first,
             "GET /one HTTP/1.1\r\nHost: a.test\r\n\r\n"
             "GET /two HTTP/1.1\r\nHost: b.test\r\n\r\n");
    pair.first = Socket{-1};

    Connection conn{std::move(pair.second)};

    const auto first = conn.next_request();
    ASSERT_TRUE(first);
    ASSERT_TRUE(first.request.has_value());
    EXPECT_EQ(first.request->target, "/one");

    const auto second = conn.next_request();
    ASSERT_TRUE(second);
    ASSERT_TRUE(second.request.has_value());
    EXPECT_EQ(second.request->target, "/two");
}

// A parse failure travels in `error`, leaving os_error clear -- the caller still
// has a socket and can answer with a status.
TEST(Connection, ReportsTheParseErrorForAMalformedHead) {
    auto pair = connected_pair();
    send_all(pair.first, "NOTAREQUEST\r\n\r\n");

    Connection conn{std::move(pair.second)};
    const auto result = conn.next_request();

    EXPECT_FALSE(result);
    EXPECT_EQ(result.error, RequestError::Malformed);
    EXPECT_EQ(result.os_error, 0);
    EXPECT_FALSE(result.request.has_value());
}

// Nothing sent and the peer hung up: not a failure, just no more requests.
TEST(Connection, ReportsNoRequestWhenTheClientClosesFirst) {
    auto pair = connected_pair();
    pair.first = Socket{-1};

    Connection conn{std::move(pair.second)};
    const auto result = conn.next_request();

    EXPECT_TRUE(result);
    EXPECT_EQ(result.error, RequestError::None);
    EXPECT_EQ(result.os_error, 0);
    EXPECT_FALSE(result.request.has_value());
}

// The recorded trade: a head cut short reads as a finished connection, since the
// peer that hung up cannot be told otherwise.
TEST(Connection, TreatsAHeadCutShortAsAFinishedConnection) {
    auto pair = connected_pair();
    send_all(pair.first, "GET /partial HTTP/1.1\r\nHost: exa");
    pair.first = Socket{-1};

    Connection conn{std::move(pair.second)};
    const auto result = conn.next_request();

    EXPECT_TRUE(result);
    EXPECT_FALSE(result.request.has_value());
}

// A read failure travels in os_error, and never as a parse error: there is no
// malformed head here, only a socket that cannot be read.
TEST(Connection, ReportsTheErrnoWhenTheReadFails) {
    Connection conn{Socket{-1}};

    const auto result = conn.next_request();

    EXPECT_FALSE(result);
    EXPECT_EQ(result.os_error, EBADF);
    EXPECT_EQ(result.error, RequestError::None);
    EXPECT_FALSE(result.request.has_value());
}

}  // namespace
