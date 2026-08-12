#include "net/listener.hpp"

#include "net/printers.hpp"
#include "net/socket.hpp"

#include <cerrno>
#include <cstdint>
#include <optional>
#include <utility>

#include <arpa/inet.h>
#include <gtest/gtest.h>
#include <sys/socket.h>

namespace {

using carafe::net::listen_on;
using carafe::net::Listener;
using carafe::net::ListenError;
using carafe::net::Socket;

// A listening socket completes the handshake on its own, so this needs no accept()
// on the other side. Nothing listening gives ECONNREFUSED instead.
bool can_connect(std::uint16_t port) {
    const Socket client{::socket(AF_INET, SOCK_STREAM, 0)};
    if (!client.valid()) {
        return false;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    const void* addr_ptr = &addr;
    return ::connect(client.get(), static_cast<const sockaddr*>(addr_ptr), sizeof(addr)) == 0;
}

// The lowest free descriptor, which is what the next socket() will be handed.
int next_free_fd() {
    const Socket probe{::socket(AF_INET, SOCK_STREAM, 0)};
    EXPECT_TRUE(probe.valid());
    return probe.get();
}

// Port 0 hands the choice to the kernel, and the result must say what it chose.
TEST(ListenOn, ChoosesAPortWhenAskedForZero) {
    const auto result = listen_on(0);

    ASSERT_TRUE(result);
    EXPECT_EQ(result.error, ListenError::None);
    EXPECT_EQ(result.os_error, 0);
    ASSERT_TRUE(result.listener.has_value());
    EXPECT_NE(result.listener->port(), 0);
}

// The one test that notices a missing ::listen: bind alone answers ECONNREFUSED.
TEST(ListenOn, LeavesTheSocketAcceptingConnections) {
    const auto result = listen_on(0);
    ASSERT_TRUE(result);

    EXPECT_TRUE(can_connect(result.listener->port()));
}

// The non-zero path skips getsockname, so it must report what it was handed.
TEST(ListenOn, ReportsTheRequestedPortWhenGivenOne) {
    std::uint16_t port = 0;
    {
        const auto ephemeral = listen_on(0);
        ASSERT_TRUE(ephemeral);
        port = ephemeral.listener->port();
    }  // released here, so the number is free to ask for by name

    const auto result = listen_on(port);
    if (!result && result.os_error == EADDRINUSE) {
        GTEST_SKIP() << "port " << port << " was taken between the two binds";
    }

    ASSERT_TRUE(result);
    EXPECT_EQ(result.listener->port(), port);
}

// SO_REUSEADDR permits binding over TIME_WAIT, not over a live listener -- that
// would need SO_REUSEPORT, which we deliberately do not set.
TEST(ListenOn, RejectsAPortAlreadyBeingListenedOn) {
    const auto held = listen_on(0);
    ASSERT_TRUE(held);

    const auto result = listen_on(held.listener->port());

    EXPECT_FALSE(result);
    EXPECT_EQ(result.error, ListenError::BindFailed);
    EXPECT_EQ(result.os_error, EADDRINUSE);
    EXPECT_FALSE(result.listener.has_value());
}

// Every failure path returns with the descriptor still owned by a local Socket.
// Leak one per call and the numbers climb; close them and the same one comes back.
TEST(ListenOn, ClosesTheDescriptorWhenBindFails) {
    const auto held = listen_on(0);
    ASSERT_TRUE(held);
    const std::uint16_t taken = held.listener->port();

    const int before = next_free_fd();
    for (int i = 0; i < 16; ++i) {
        ASSERT_EQ(listen_on(taken).error, ListenError::BindFailed);
    }

    EXPECT_EQ(next_free_fd(), before);
}

// Moving must carry the socket whole: destroying the source afterwards is what
// closes the descriptor if it did not.
TEST(Listener, MoveKeepsTheSocketListening) {
    auto source = listen_on(0);
    ASSERT_TRUE(source);
    const std::uint16_t port = source.listener->port();

    const Listener moved = std::move(*source.listener);
    source.listener.reset();

    EXPECT_EQ(moved.port(), port);
    EXPECT_TRUE(can_connect(port));
}

}  // namespace
