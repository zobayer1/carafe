#include "net/socket.hpp"

#include "alarm.hpp"
#include "net/listener.hpp"

#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <fcntl.h>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <arpa/inet.h>
#include <gtest/gtest.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>

namespace {

using carafe::net::milliseconds_until;
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

// Long enough that a write bounded by it is bounded by nothing these tests are about. The deadline tests name their
// own.
constexpr auto no_hurry = std::chrono::seconds(5);

// A connected pair over the loopback, with the buffers pinned small so a slow reader holds the sender back. A
// socketpair cannot stand in here: AF_UNIX takes a whole payload into a single send, so the write loop never goes round
// twice and a deadline on the loop cannot be told apart from a deadline on one send. Invalid sockets mean the setup
// failed, which the caller checks before relying on either.
std::pair<Socket, Socket> loopback_pair() {
    auto listening = carafe::net::listen_on(0);
    if (!listening) {
        return {Socket{-1}, Socket{-1}};
    }

    Socket client{::socket(AF_INET, SOCK_STREAM, 0)};
    const int small = 16 * 1024;
    EXPECT_EQ(::setsockopt(client.get(), SOL_SOCKET, SO_RCVBUF, &small, sizeof(small)), 0);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(listening.listener->port());
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    const void* addr_ptr = &addr;
    if (::connect(client.get(), static_cast<const sockaddr*>(addr_ptr), sizeof(addr)) != 0) {
        return {Socket{-1}, Socket{-1}};
    }

    auto accepted = listening.listener->accept();
    if (!accepted) {
        return {Socket{-1}, Socket{-1}};
    }
    EXPECT_EQ(::setsockopt(accepted.client->get(), SOL_SOCKET, SO_SNDBUF, &small, sizeof(small)), 0);

    return {std::move(client), std::move(*accepted.client)};
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

    ASSERT_TRUE(writer.write("hello", no_hurry));

    std::array<char, 64> buf{};
    const auto result = reader.read(buf.data(), buf.size());
    ASSERT_TRUE(result.bytes.has_value());
    EXPECT_EQ(*result.bytes, "hello");
}

// Nothing to send is success, and must not reach send(), where a zero count would leave the loop with no progress to
// make.
TEST(SocketWrite, SucceedsWithoutSendingAnythingWhenGivenNoBytes) {
    auto [reader, writer] = connected_pair();

    EXPECT_TRUE(writer.write("", no_hurry));

    char byte = 0;
    EXPECT_EQ(::recv(reader.get(), &byte, 1, MSG_DONTWAIT), ssize_t{-1});
    EXPECT_EQ(errno, EAGAIN);
}

// The MSG_NOSIGNAL test. Without that flag this does not fail: SIGPIPE kills the whole process, which is how a server
// dies when one client hangs up early.
TEST(SocketWrite, ReportsBrokenPipeWhenThePeerIsGone) {
    auto [reader, writer] = connected_pair();
    reader = Socket{-1};

    const auto result = writer.write("anyone there?", no_hurry);

    EXPECT_FALSE(result);
    EXPECT_EQ(result.os_error, EPIPE);
}

// Failure carries the errno and nothing else, so it cannot be mistaken for success.
TEST(SocketWrite, ReportsTheErrnoWhenTheDescriptorIsInvalid) {
    Socket sock{-1};

    const auto result = sock.write("x", no_hurry);

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

    const auto result = writer.write(payload, no_hurry);
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
        result = writer.write(payload, no_hurry);
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
        result = writer.write("!", no_hurry);
    }
    sink.join();

    const int delivered = alarms_delivered;
    EXPECT_GE(delivered, 1) << "no signal arrived, so nothing was interrupted";
    EXPECT_TRUE(result);
    ASSERT_EQ(received.size(), stuffed + 1);
    EXPECT_EQ(received.back(), '!');
}

// Without a deadline a read on an idle socket waits for as long as the peer cares to say nothing, which is a thread
// and a descriptor held for free.
TEST(SocketReceiveTimeout, ReportsEagainWhenNothingArrivesInTime) {
    auto pair = connected_pair();
    ASSERT_EQ(pair.first.set_receive_timeout(std::chrono::milliseconds(50)), 0);

    std::array<char, 64> buf{};
    const ReadResult result = pair.first.read(buf.data(), buf.size());

    EXPECT_FALSE(result);
    EXPECT_FALSE(result.bytes.has_value());
    EXPECT_TRUE(result.os_error == EAGAIN || result.os_error == EWOULDBLOCK) << "errno " << result.os_error;
}

// The caller is told, because a connection whose reads cannot be bounded is one the server would rather not take.
TEST(SocketReceiveTimeout, ReportsFailureWhenTheDescriptorIsInvalid) {
    Socket empty{-1};
    // The errno survives, which is what the caller needs to report a connection it cannot bound.
    EXPECT_EQ(empty.set_receive_timeout(std::chrono::milliseconds(50)), EBADF);
}

// The deadline bounds waiting, not reading: bytes already there are answered at once.
TEST(SocketReceiveTimeout, LeavesAReadWithBytesWaitingAlone) {
    auto pair = connected_pair();
    ASSERT_EQ(pair.first.set_receive_timeout(std::chrono::milliseconds(50)), 0);
    ASSERT_TRUE(pair.second.write("hi", no_hurry));

    std::array<char, 64> buf{};
    const ReadResult result = pair.first.read(buf.data(), buf.size());

    ASSERT_TRUE(result) << "errno " << result.os_error;
    EXPECT_EQ(result.bytes, "hi");
}

// The deadline is per recv rather than per connection, which is what leaves a slow drip unbounded: every byte that
// arrives buys the sender the whole interval again.
TEST(SocketReceiveTimeout, StartsOverWheneverAByteArrives) {
    auto pair = connected_pair();
    ASSERT_EQ(pair.first.set_receive_timeout(std::chrono::milliseconds(80)), 0);

    std::thread drip([&pair] {
        for (int i = 0; i < 3; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(40));
            EXPECT_TRUE(pair.second.write("x", no_hurry));
        }
    });

    std::array<char, 64> buf{};
    for (int i = 0; i < 3; ++i) {
        const ReadResult result = pair.first.read(buf.data(), buf.size());
        EXPECT_TRUE(result) << "read " << i << " errno " << result.os_error;
    }
    drip.join();
}

// The mirror of the receive deadline: a peer that stops reading fills the buffers and then holds the sender for as long
// as it likes, which is a thread held by someone sending nothing at all.
TEST(SocketSendTimeout, GivesUpWhenThePeerStopsReading) {
    auto pair = connected_pair();

    // Far more than any socket buffer holds, with nobody on the other end reading a byte of it.
    const std::string more_than_fits(std::size_t{4} * 1024 * 1024, 'x');
    const auto started = std::chrono::steady_clock::now();
    const WriteResult result = pair.first.write(more_than_fits, std::chrono::milliseconds(50));

    EXPECT_FALSE(result);
    EXPECT_TRUE(result.os_error == EAGAIN || result.os_error == EWOULDBLOCK || result.os_error == ETIMEDOUT)
        << "errno " << result.os_error;
    EXPECT_LT(std::chrono::steady_clock::now() - started, std::chrono::seconds(1));
}

// What a per-send deadline leaves unbounded. A blocking send comes back short only when something interrupts it after
// it has moved bytes, and its own deadline firing is exactly that: the write loop goes round, hands the next send the
// whole limit again, and a peer draining steadily but slowly renews it for as long as it likes. Measuring from the
// first send is what ends it.
TEST(SocketSendTimeout, GivesUpWhenThePeerDrainsTooSlowly) {
    auto pair = loopback_pair();
    Socket& reader = pair.first;
    Socket& writer = pair.second;
    ASSERT_TRUE(reader.valid() && writer.valid());

    std::atomic<bool> reading{true};
    std::thread sipper([&reader, &reading] {
        std::vector<char> buf(std::size_t{64} * 1024);
        while (reading) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            static_cast<void>(::recv(reader.get(), buf.data(), buf.size(), MSG_DONTWAIT));
        }
    });

    const std::string more_than_fits(std::size_t{2} * 1024 * 1024, 'x');
    const auto started = std::chrono::steady_clock::now();
    const WriteResult result = writer.write(more_than_fits, std::chrono::milliseconds(100));
    const auto took = std::chrono::steady_clock::now() - started;

    reading = false;
    sipper.join();

    EXPECT_FALSE(result);
    EXPECT_LT(took, std::chrono::seconds(1)) << "the write outlived the deadline it was given";
}

// Nothing rather than zero, because a zero timeval asks setsockopt for no deadline at all: the last fraction of a
// millisecond would become an unbounded wait.
TEST(MillisecondsUntil, ReportsNothingWhenUnderAMillisecondIsLeft) {
    EXPECT_FALSE(milliseconds_until(std::chrono::steady_clock::now() + std::chrono::microseconds(200)).has_value());
}

TEST(MillisecondsUntil, ReportsNothingWhenTheDeadlineHasPassed) {
    EXPECT_FALSE(milliseconds_until(std::chrono::steady_clock::now() - std::chrono::seconds(1)).has_value());
}

// Rounded down, so what setsockopt is handed never reaches past the deadline it came from.
TEST(MillisecondsUntil, ReportsWhatIsLeftOfTheDeadline) {
    const auto left = milliseconds_until(std::chrono::steady_clock::now() + std::chrono::milliseconds(500));

    ASSERT_TRUE(left.has_value());
    EXPECT_GT(*left, std::chrono::milliseconds(400));
    EXPECT_LE(*left, std::chrono::milliseconds(500));
}

}  // namespace
