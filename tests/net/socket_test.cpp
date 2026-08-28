#include "net/socket.hpp"

#include "alarm.hpp"

#include <array>
#include <cerrno>
#include <chrono>
#include <fcntl.h>
#include <string>
#include <thread>
#include <utility>

#include <gtest/gtest.h>
#include <sys/socket.h>
#include <sys/time.h>

namespace {

using carafe::net::ReadResult;
using carafe::net::Socket;
using carafe::net::WriteResult;
using carafe::test::AlarmIn;
using carafe::test::alarms_delivered;
using carafe::test::mask_alarm;

// AF_UNIX needs no address, port, or network stack: the cheapest real descriptor, and what it connects to never matters
// here.
int make_fd() {
    const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    EXPECT_NE(fd, -1);
    return fd;
}

// Reads the descriptor table without touching the descriptor: closed is EBADF.
bool is_open(int fd) {
    return ::fcntl(fd, F_GETFD) != -1;
}

// Two connected sockets with no address, port, or listener in sight: read only needs something with a far end.
std::pair<Socket, Socket> connected_pair() {
    std::array<int, 2> fds{-1, -1};
    EXPECT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds.data()), 0);
    return {Socket{fds[0]}, Socket{fds[1]}};
}

// Distinctive content, so a prefix sent twice shows up as a mismatch instead of as bytes that happen to look right.
std::string pattern(std::size_t size) {
    std::string out(size, '\0');
    for (std::size_t i = 0; i < size; ++i) {
        out[i] = static_cast<char>('a' + (i % 26));
    }
    return out;
}

// Stuffs the socket buffer until it will take no more, so the next send has nowhere to put anything and blocks before
// moving a single byte.
std::size_t fill_send_buffer(const Socket& sock) {
    std::size_t total = 0;
    const std::array<char, 4096> block{};
    while (true) {
        const ssize_t sent = ::send(sock.get(), block.data(), block.size(), MSG_DONTWAIT | MSG_NOSIGNAL);
        if (sent == -1) {
            return total;
        }
        total += static_cast<std::size_t>(sent);
    }
}

// A reader that gives up eventually. Without this, a write that stops early leaves the draining thread blocked forever,
// so the suite hangs where it should go red.
void set_read_timeout(const Socket& sock, int millis) {
    timeval timeout{};
    timeout.tv_sec = millis / 1000;
    timeout.tv_usec = suseconds_t{millis % 1000} * 1000;
    EXPECT_EQ(::setsockopt(sock.get(), SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)), 0);
}

// Keeps reading until `total` bytes have arrived, or until a read gives up.
std::string drain(Socket& sock, std::size_t total) {
    std::string received;
    received.reserve(total);

    std::array<char, 4096> buf{};
    while (received.size() < total) {
        const auto result = sock.read(buf.data(), buf.size());
        if (!result || !result.bytes.has_value()) {
            break;
        }
        received.append(*result.bytes);
    }
    return received;
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
    // Inspecting a moved-from Socket is the point: this class specifies that state as empty, where the standard library
    // only promises "unspecified".
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

// Unguarded, this closes the descriptor and then takes it straight back. Written through a reference because
// -Wself-move rejects the direct spelling.
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

// Short reads are the norm, not an edge case: asking for 4096 does not wait for 4096, which is why RequestReader is fed
// incrementally.
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

// The alarm fires while read is blocked and the bytes arrive only later, so without the retry loop this returns EINTR
// instead of data.
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

TEST(SocketWrite, SendsEveryByte) {
    auto [reader, writer] = connected_pair();

    ASSERT_TRUE(writer.write("hello"));

    std::array<char, 64> buf{};
    const auto result = reader.read(buf.data(), buf.size());
    ASSERT_TRUE(result.bytes.has_value());
    EXPECT_EQ(*result.bytes, "hello");
}

// Nothing to send is success, and must not reach send(), where a zero count would leave the loop with no progress to
// make.
TEST(SocketWrite, SucceedsWithoutSendingAnythingWhenGivenNoBytes) {
    auto [reader, writer] = connected_pair();

    EXPECT_TRUE(writer.write(""));

    char byte = 0;
    EXPECT_EQ(::recv(reader.get(), &byte, 1, MSG_DONTWAIT), ssize_t{-1});
    EXPECT_EQ(errno, EAGAIN);
}

// The MSG_NOSIGNAL test. Without that flag this does not fail: SIGPIPE kills the whole process, which is how a server
// dies when one client hangs up early.
TEST(SocketWrite, ReportsBrokenPipeWhenThePeerIsGone) {
    auto [reader, writer] = connected_pair();
    reader = Socket{-1};

    const auto result = writer.write("anyone there?");

    EXPECT_FALSE(result);
    EXPECT_EQ(result.os_error, EPIPE);
}

// Failure carries the errno and nothing else, so it cannot be mistaken for success.
TEST(SocketWrite, ReportsTheErrnoWhenTheDescriptorIsInvalid) {
    Socket sock{-1};

    const auto result = sock.write("x");

    EXPECT_FALSE(result);
    EXPECT_EQ(result.os_error, EBADF);
}

// More than the socket buffer holds, so the call cannot complete until the far end is drained. Every byte must still
// arrive, exactly once and in order.
TEST(SocketWrite, SendsMoreThanTheSocketBufferHolds) {
    auto pair = connected_pair();
    Socket& reader = pair.first;
    Socket& writer = pair.second;
    set_read_timeout(reader, 2000);
    const std::string payload = pattern(1U << 20);

    std::string received;
    std::thread sink([&reader, &received, size = payload.size()] { received = drain(reader, size); });

    const auto result = writer.write(payload);
    sink.join();

    ASSERT_TRUE(result);
    EXPECT_EQ(received.size(), payload.size());
    EXPECT_EQ(received, payload);
}

// A signal landing after send has already moved bytes comes back as a short count, not EINTR, so this is the resume
// path rather than the retry one. Resuming from the wrong offset would put a prefix on the wire twice.
TEST(SocketWrite, ResumesFromWhereASignalStoppedIt) {
    auto pair = connected_pair();
    Socket& reader = pair.first;
    Socket& writer = pair.second;
    set_read_timeout(reader, 2000);
    const std::string payload = pattern(1U << 20);

    mask_alarm(SIG_BLOCK);
    std::string received;
    std::thread sink([&reader, &received, size = payload.size()] {
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        received = drain(reader, size);
    });
    mask_alarm(SIG_UNBLOCK);

    WriteResult result;
    {
        const AlarmIn alarm(50000);
        result = writer.write(payload);
    }
    sink.join();

    const int delivered = alarms_delivered;
    EXPECT_GE(delivered, 1) << "no signal arrived, so nothing was interrupted";
    EXPECT_TRUE(result);
    EXPECT_EQ(received.size(), payload.size());
    EXPECT_EQ(received, payload);
}

// EINTR is only reachable when the signal beats the first byte out, so the buffer is stuffed full first and the write
// has nowhere to put even one.
TEST(SocketWrite, RetriesWhenASignalArrivesBeforeAnyByteMoves) {
    auto pair = connected_pair();
    Socket& reader = pair.first;
    Socket& writer = pair.second;
    set_read_timeout(reader, 2000);

    const std::size_t stuffed = fill_send_buffer(writer);
    ASSERT_GT(stuffed, 0U);

    mask_alarm(SIG_BLOCK);
    std::string received;
    std::thread sink([&reader, &received, total = stuffed + 1] {
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        received = drain(reader, total);
    });
    mask_alarm(SIG_UNBLOCK);

    WriteResult result;
    {
        const AlarmIn alarm(50000);
        result = writer.write("!");
    }
    sink.join();

    const int delivered = alarms_delivered;
    EXPECT_GE(delivered, 1) << "no signal arrived, so nothing was interrupted";
    EXPECT_TRUE(result);
    ASSERT_EQ(received.size(), stuffed + 1);
    EXPECT_EQ(received.back(), '!');
}

}  // namespace
