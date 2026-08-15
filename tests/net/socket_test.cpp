#include "net/socket.hpp"

#include "alarm.hpp"

#include <array>
#include <cerrno>
#include <chrono>
#include <fcntl.h>
#include <thread>
#include <utility>

#include <gtest/gtest.h>
#include <sys/socket.h>

namespace {

using carafe::net::ReadResult;
using carafe::net::Socket;
using carafe::test::AlarmIn;
using carafe::test::alarms_delivered;
using carafe::test::mask_alarm;

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

// Two connected sockets with no address, port, or listener in sight -- read only
// needs something with a far end.
std::pair<Socket, Socket> connected_pair() {
    std::array<int, 2> fds{-1, -1};
    EXPECT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds.data()), 0);
    return {Socket{fds[0]}, Socket{fds[1]}};
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

TEST(SocketRead, ReturnsWhatThePeerSent) {
    auto [reader, writer] = connected_pair();
    ASSERT_EQ(::send(writer.get(), "hello", 5, 0), ssize_t{5});

    std::array<char, 64> buf{};
    const auto result = reader.read(buf.data(), buf.size());

    ASSERT_TRUE(result);
    EXPECT_EQ(result.os_error, 0);
    ASSERT_TRUE(result.bytes.has_value());
    EXPECT_EQ(*result.bytes, "hello");
}

// The contract the header states: no copy, so the view dies with the buffer.
TEST(SocketRead, ViewsTheCallersBufferRatherThanACopy) {
    auto [reader, writer] = connected_pair();
    ASSERT_EQ(::send(writer.get(), "abc", 3, 0), ssize_t{3});

    std::array<char, 64> buf{};
    const auto result = reader.read(buf.data(), buf.size());

    ASSERT_TRUE(result.bytes.has_value());
    EXPECT_EQ(result.bytes->data(), buf.data());
}

// Short reads are the norm, not an edge case: asking for 4096 does not wait for
// 4096, which is why RequestReader is fed incrementally.
TEST(SocketRead, ReturnsOnlyWhatHasArrived) {
    auto [reader, writer] = connected_pair();
    ASSERT_EQ(::send(writer.get(), "ab", 2, 0), ssize_t{2});

    std::array<char, 4096> buf{};
    const auto result = reader.read(buf.data(), buf.size());

    ASSERT_TRUE(result.bytes.has_value());
    EXPECT_EQ(result.bytes->size(), 2U);
}

// A clean close is how well-behaved peers finish, so it must not look like failure.
TEST(SocketRead, ReportsEndOfStreamWhenThePeerCloses) {
    auto [reader, writer] = connected_pair();
    writer = Socket{-1};  // closes the far end

    std::array<char, 64> buf{};
    const auto result = reader.read(buf.data(), buf.size());

    EXPECT_TRUE(result);
    EXPECT_EQ(result.os_error, 0);
    EXPECT_FALSE(result.bytes.has_value());
}

// Bytes already in flight outrank the close: EOF is what is left afterwards.
TEST(SocketRead, DeliversBufferedBytesBeforeReportingEndOfStream) {
    auto [reader, writer] = connected_pair();
    ASSERT_EQ(::send(writer.get(), "last", 4, 0), ssize_t{4});
    writer = Socket{-1};

    std::array<char, 64> buf{};
    const auto first = reader.read(buf.data(), buf.size());
    ASSERT_TRUE(first.bytes.has_value());
    EXPECT_EQ(*first.bytes, "last");

    const auto second = reader.read(buf.data(), buf.size());
    EXPECT_TRUE(second);
    EXPECT_FALSE(second.bytes.has_value());
}

// Failure keeps errno and hands back nothing, so the two are never confused.
TEST(SocketRead, ReportsTheErrnoWhenTheDescriptorIsInvalid) {
    Socket sock{-1};

    std::array<char, 64> buf{};
    const auto result = sock.read(buf.data(), buf.size());

    EXPECT_FALSE(result);
    EXPECT_EQ(result.os_error, EBADF);
    EXPECT_FALSE(result.bytes.has_value());
}

// The alarm fires while read is blocked and the bytes arrive only later, so
// without the retry loop this returns EINTR instead of data.
TEST(SocketRead, KeepsWaitingWhenASignalInterruptsIt) {
    auto [reader, writer] = connected_pair();
    const int writer_fd = writer.get();

    mask_alarm(SIG_BLOCK);
    std::thread sender([writer_fd] {
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        EXPECT_EQ(::send(writer_fd, "late", 4, 0), ssize_t{4});
    });
    mask_alarm(SIG_UNBLOCK);

    std::array<char, 64> buf{};
    ReadResult result;
    {
        const AlarmIn alarm(50000);  // fires while read waits, long before the bytes
        result = reader.read(buf.data(), buf.size());
    }
    sender.join();

    const int delivered = alarms_delivered;
    EXPECT_GE(delivered, 1) << "no signal arrived, so nothing was interrupted";
    ASSERT_TRUE(result);
    ASSERT_TRUE(result.bytes.has_value());
    EXPECT_EQ(*result.bytes, "late");
}

}  // namespace
