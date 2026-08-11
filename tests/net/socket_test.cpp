#include "net/socket.hpp"

#include <fcntl.h>
#include <utility>

#include <gtest/gtest.h>
#include <sys/socket.h>

namespace {

using carafe::net::Socket;

// AF_UNIX needs no address, port, or network stack -- the cheapest real
// descriptor, and what it connects to never matters here.
int make_fd() {
    const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    EXPECT_NE(fd, -1);
    return fd;
}

// Reads the descriptor table without touching the descriptor: closed is EBADF.
bool is_open(int fd) {
    return ::fcntl(fd, F_GETFD) != -1;
}

// The constructor takes ownership of what it is given and hides nothing.
TEST(Socket, AdoptsTheDescriptorItIsGiven) {
    const int fd = make_fd();
    const Socket sock{fd};

    EXPECT_EQ(sock.get(), fd);
    EXPECT_TRUE(sock.valid());
}

// The whole reason the class exists: leaving scope releases the descriptor.
TEST(Socket, ClosesTheDescriptorOnDestruction) {
    const int fd = make_fd();
    ASSERT_TRUE(is_open(fd));

    {
        const Socket sock{fd};
    }

    // Asked immediately: the number is free now and the next open() may take it.
    EXPECT_FALSE(is_open(fd));
}

// -1 is a legal input, and such a Socket must close nothing.
TEST(Socket, OwnsNothingWhenGivenTheSentinel) {
    const Socket sock{-1};

    EXPECT_FALSE(sock.valid());
    EXPECT_EQ(sock.get(), -1);
}

// Ownership moves whole, and nothing closes on the way.
TEST(Socket, MoveConstructionTransfersTheDescriptor) {
    const int fd = make_fd();
    Socket source{fd};
    const Socket target{std::move(source)};

    EXPECT_EQ(target.get(), fd);
    EXPECT_TRUE(is_open(fd));
    // Inspecting a moved-from Socket is the point: this class specifies that
    // state as empty, where the standard library only promises "unspecified".
    // NOLINTNEXTLINE(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
    EXPECT_FALSE(source.valid());
}

// Emptying the source is what keeps its destructor off the target's descriptor.
TEST(Socket, MoveConstructionEmptiesTheSourceBeforeItIsDestroyed) {
    const int fd = make_fd();
    Socket source{fd};  // declared first, so destroyed last

    {
        const Socket target{std::move(source)};
    }

    EXPECT_FALSE(is_open(fd));
    // NOLINTNEXTLINE(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
    EXPECT_FALSE(source.valid());
}

// Dropping the replaced descriptor would leak it for the life of the process.
TEST(Socket, MoveAssignmentClosesTheDescriptorItReplaces) {
    const int replaced = make_fd();
    const int adopted = make_fd();

    Socket target{replaced};
    Socket source{adopted};
    target = std::move(source);

    EXPECT_FALSE(is_open(replaced));
    EXPECT_TRUE(is_open(adopted));
    EXPECT_EQ(target.get(), adopted);
    // NOLINTNEXTLINE(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
    EXPECT_FALSE(source.valid());
}

// Replacement is unconditional: an empty source empties the target.
TEST(Socket, MoveAssignmentFromAnEmptySocketClosesTheTarget) {
    const int fd = make_fd();

    Socket target{fd};
    Socket source{-1};
    target = std::move(source);

    EXPECT_FALSE(is_open(fd));
    EXPECT_FALSE(target.valid());
}

// Unguarded, this closes the descriptor and then takes it straight back.
// Written through a reference because -Wself-move rejects the direct spelling.
TEST(Socket, SelfMoveAssignmentKeepsTheDescriptor) {
    const int fd = make_fd();
    Socket sock{fd};

    Socket& alias = sock;
    sock = std::move(alias);

    EXPECT_TRUE(is_open(fd));
    EXPECT_EQ(sock.get(), fd);
    EXPECT_TRUE(sock.valid());
}

}  // namespace
