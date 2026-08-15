#include "net/listener.hpp"

#include "net/printers.hpp"
#include "net/socket.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <fcntl.h>
#include <optional>
#include <pthread.h>
#include <thread>
#include <utility>

#include <arpa/inet.h>
#include <gtest/gtest.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/time.h>

namespace {

using carafe::net::AcceptResult;
using carafe::net::listen_on;
using carafe::net::Listener;
using carafe::net::ListenError;
using carafe::net::Socket;

// The handshake completes without an accept() on the other side, so the caller gets
// a live client back. An invalid Socket means nothing was listening.
Socket connect_to(std::uint16_t port) {
    Socket client{::socket(AF_INET, SOCK_STREAM, 0)};
    if (!client.valid()) {
        return client;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    const void* addr_ptr = &addr;
    if (::connect(client.get(), static_cast<const sockaddr*>(addr_ptr), sizeof(addr)) != 0) {
        return Socket{-1};
    }
    return client;
}

// Hangs up immediately, which is all the listen_on tests need to know.
bool can_connect(std::uint16_t port) {
    return connect_to(port).valid();
}

// One byte each way is enough to tell which connection a Socket is the far end of.
void send_byte(const Socket& sock, char byte) {
    EXPECT_EQ(::send(sock.get(), &byte, 1, 0), ssize_t{1});
}

char read_byte(const Socket& sock) {
    char byte = 0;
    EXPECT_EQ(::recv(sock.get(), &byte, 1, 0), ssize_t{1});
    return byte;
}

// A handler can report back only through a global of this type.
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
volatile std::sig_atomic_t alarms_delivered = 0;

extern "C" void count_alarm(int /*signal*/) {
    alarms_delivered = alarms_delivered + 1;
}

// A thread created while SIGALRM is blocked inherits that, so only this one is
// ever interrupted.
void mask_alarm(int how) {
    sigset_t alarm_only;
    sigemptyset(&alarm_only);
    sigaddset(&alarm_only, SIGALRM);
    EXPECT_EQ(pthread_sigmask(how, &alarm_only, nullptr), 0);
}

// One-shot SIGALRM, handler and timer restored on the way out. No SA_RESTART, so
// the kernel hands the interrupted call back as EINTR instead of resuming it.
class AlarmIn {
public:
    explicit AlarmIn(suseconds_t micros) {
        struct sigaction handler{};
        handler.sa_handler = count_alarm;
        handler.sa_flags = 0;
        sigemptyset(&handler.sa_mask);
        EXPECT_EQ(::sigaction(SIGALRM, &handler, &previous_), 0);

        alarms_delivered = 0;
        itimerval timer{};
        timer.it_value.tv_usec = micros;
        EXPECT_EQ(::setitimer(ITIMER_REAL, &timer, nullptr), 0);
    }

    ~AlarmIn() {
        const itimerval disarm{};
        ::setitimer(ITIMER_REAL, &disarm, nullptr);
        ::sigaction(SIGALRM, &previous_, nullptr);
    }

    AlarmIn(const AlarmIn&) = delete;
    AlarmIn& operator=(const AlarmIn&) = delete;
    AlarmIn(AlarmIn&&) = delete;
    AlarmIn& operator=(AlarmIn&&) = delete;

private:
    struct sigaction previous_{};
};

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

// The handshake is already queued, so accept has a client waiting and never blocks.
TEST(Accept, ReturnsASocketForAWaitingClient) {
    auto held = listen_on(0);
    ASSERT_TRUE(held);

    const Socket client = connect_to(held.listener->port());
    ASSERT_TRUE(client.valid());

    const auto result = held.listener->accept();

    ASSERT_TRUE(result);
    EXPECT_EQ(result.os_error, 0);
    ASSERT_TRUE(result.client.has_value());
    EXPECT_TRUE(result.client->valid());
}

// A valid descriptor is not enough -- it has to be the far end of this connection.
TEST(Accept, ReadsWhatTheClientSent) {
    auto held = listen_on(0);
    ASSERT_TRUE(held);

    const Socket client = connect_to(held.listener->port());
    ASSERT_TRUE(client.valid());
    send_byte(client, 'x');

    const auto result = held.listener->accept();
    ASSERT_TRUE(result);

    EXPECT_EQ(read_byte(*result.client), 'x');
}

// Two accepts must yield two connections, not the same one twice.
TEST(Accept, GivesEachClientItsOwnSocket) {
    auto held = listen_on(0);
    ASSERT_TRUE(held);
    const std::uint16_t port = held.listener->port();

    const Socket first = connect_to(port);
    const Socket second = connect_to(port);
    ASSERT_TRUE(first.valid());
    ASSERT_TRUE(second.valid());
    send_byte(first, 'a');
    send_byte(second, 'b');

    const auto one = held.listener->accept();
    const auto two = held.listener->accept();
    ASSERT_TRUE(one);
    ASSERT_TRUE(two);
    EXPECT_NE(one.client->get(), two.client->get());

    const char from_one = read_byte(*one.client);
    const char from_two = read_byte(*two.client);
    EXPECT_EQ(std::min(from_one, from_two), 'a');
    EXPECT_EQ(std::max(from_one, from_two), 'b');
}

// Flags are not inherited across accept: only accept4 puts CLOEXEC on the client.
TEST(Accept, SetsCloseOnExecOnTheAcceptedSocket) {
    auto held = listen_on(0);
    ASSERT_TRUE(held);

    const Socket client = connect_to(held.listener->port());
    ASSERT_TRUE(client.valid());

    const auto result = held.listener->accept();
    ASSERT_TRUE(result);

    const int flags = ::fcntl(result.client->get(), F_GETFD);
    ASSERT_NE(flags, -1);
    EXPECT_NE(flags & FD_CLOEXEC, 0);
}

// The one failure reachable without fault injection: no room left for the client fd.
TEST(Accept, ReportsTheErrnoWhenNoDescriptorIsAvailable) {
    auto held = listen_on(0);
    ASSERT_TRUE(held);

    const Socket client = connect_to(held.listener->port());
    ASSERT_TRUE(client.valid());

    rlimit original{};
    ASSERT_EQ(::getrlimit(RLIMIT_NOFILE, &original), 0);
    rlimit tightened = original;
    tightened.rlim_cur = static_cast<rlim_t>(next_free_fd());
    ASSERT_EQ(::setrlimit(RLIMIT_NOFILE, &tightened), 0);

    const auto result = held.listener->accept();

    // Restored before the assertions, which need descriptors of their own to report.
    ASSERT_EQ(::setrlimit(RLIMIT_NOFILE, &original), 0);

    EXPECT_FALSE(result);
    EXPECT_FALSE(result.client.has_value());
    EXPECT_EQ(result.os_error, EMFILE);
}

// The alarm fires while accept is blocked and the client arrives only later, so
// without the retry loop this returns EINTR instead of a connection.
TEST(Accept, KeepsWaitingWhenASignalInterruptsIt) {
    auto held = listen_on(0);
    ASSERT_TRUE(held);

    mask_alarm(SIG_BLOCK);
    std::thread connector([port = held.listener->port()] {
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        const Socket client = connect_to(port);
        EXPECT_TRUE(client.valid());
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    });
    mask_alarm(SIG_UNBLOCK);

    AcceptResult result;
    {
        const AlarmIn alarm(50000);  // fires while accept waits, long before the client
        result = held.listener->accept();
    }
    connector.join();

    const int delivered = alarms_delivered;
    EXPECT_GE(delivered, 1) << "no signal arrived, so nothing was interrupted";
    EXPECT_TRUE(result);
    EXPECT_EQ(result.os_error, 0);
}

}  // namespace
