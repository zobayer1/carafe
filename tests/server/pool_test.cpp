#include "server/pool.hpp"

#include <carafe/config.hpp>
#include <carafe/http/handler.hpp>
#include <carafe/http/request.hpp>
#include <carafe/http/response.hpp>

#include "net/socket.hpp"
#include "server/connection.hpp"
#include "server/pipeline.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <gtest/gtest.h>
#include <sys/socket.h>
#include <sys/time.h>

// A sanitizer build brings its own allocator and checks that each allocation is released through the operator that
// matches it. Replacing operator new here would break that pairing, since gtest takes the nothrow form, and it would
// blunt the same checking for the library. So the device below is left out of those builds, and the one test that
// needs it skips.
#ifdef __SANITIZE_ADDRESS__
#define CARAFE_TEST_WITHOUT_ALLOCATION_FAILURE 1
#endif
#ifdef __has_feature
#if __has_feature(address_sanitizer)
#define CARAFE_TEST_WITHOUT_ALLOCATION_FAILURE 1
#endif
#endif

#ifndef CARAFE_TEST_WITHOUT_ALLOCATION_FAILURE

namespace {

// Zero disarms. Armed, the next allocation at or above this size fails once, and only that one. Replacing operator new
// reaches every test in this binary, which is why it does nothing until a test asks for a failure and why it disarms
// itself the moment it delivers one.
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
std::atomic<std::size_t> fail_allocation_of_at_least{0};

// Set when an armed failure is delivered. A test that has to fail whichever allocation a call happens to make needs to
// know whether the call made one at all, or it asserts nothing on the day the call stops allocating.
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
std::atomic<bool> allocation_failed{false};

}  // namespace

void* operator new(std::size_t size) {
    const std::size_t threshold = fail_allocation_of_at_least.load(std::memory_order_relaxed);
    if (threshold != 0 && size >= threshold) {
        fail_allocation_of_at_least.store(0, std::memory_order_relaxed);
        allocation_failed.store(true, std::memory_order_relaxed);
        throw std::bad_alloc{};
    }

    // NOLINTNEXTLINE(cppcoreguidelines-no-malloc,cppcoreguidelines-owning-memory)
    void* memory = std::malloc(size);
    if (memory == nullptr) {
        throw std::bad_alloc{};
    }
    return memory;
}

void operator delete(void* memory) noexcept {
    // NOLINTNEXTLINE(cppcoreguidelines-no-malloc,cppcoreguidelines-owning-memory)
    std::free(memory);
}

void operator delete(void* memory, std::size_t /*size*/) noexcept {
    // NOLINTNEXTLINE(cppcoreguidelines-no-malloc,cppcoreguidelines-owning-memory)
    std::free(memory);
}

#endif  // CARAFE_TEST_WITHOUT_ALLOCATION_FAILURE

namespace {

using carafe::Deadlines;
using carafe::PoolLimits;
using carafe::http::Handler;
using carafe::http::Method;
using carafe::http::Request;
using carafe::http::text_response;
using carafe::net::Socket;
using carafe::server::ConnectionPool;
using carafe::server::Pipeline;

// Stuffs a socket until it will take nothing more, so a refusal written to it has nowhere to go.
std::size_t fill(const Socket& sock) {
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

#ifndef CARAFE_TEST_WITHOUT_ALLOCATION_FAILURE

void fail_next_allocation_of_at_least(std::size_t size) {
    allocation_failed.store(false, std::memory_order_relaxed);
    fail_allocation_of_at_least.store(size, std::memory_order_relaxed);
}

bool an_allocation_failed() {
    return allocation_failed.load(std::memory_order_relaxed);
}

void stop_failing_allocations() {
    fail_allocation_of_at_least.store(0, std::memory_order_relaxed);
}

// Sends what the socket takes and stops without complaint when the far end goes: a connection dropped part way through
// a body is what one test here arranges on purpose.
void send_until_closed(const Socket& sock, std::string_view bytes) {
    while (!bytes.empty()) {
        const ssize_t sent = ::send(sock.get(), bytes.data(), bytes.size(), MSG_NOSIGNAL);
        if (sent == -1) {
            return;
        }
        bytes.remove_prefix(static_cast<std::size_t>(sent));
    }
}

#endif  // CARAFE_TEST_WITHOUT_ALLOCATION_FAILURE

// Short enough that no test waits out a default, long enough that a handler held for a moment is not cut off under it.
constexpr Deadlines brief{std::chrono::milliseconds(500), std::chrono::milliseconds(500)};

// Room for everything each test submits, so only the limit a test is about can be the one that fires.
constexpr PoolLimits roomy{4, 8, std::chrono::seconds(5)};

constexpr std::string_view get_root = "GET / HTTP/1.1\r\nHost: example.test\r\n\r\n";

// AF_UNIX: the pool wants a byte stream with a far end, not a network. The client half carries a deadline of its own so
// a test waiting on a response the pool never sent fails rather than hangs.
std::pair<Socket, Socket> connected_pair() {
    std::array<int, 2> fds{-1, -1};
    EXPECT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds.data()), 0);

    const timeval deadline{2, 0};
    EXPECT_EQ(::setsockopt(fds[0], SOL_SOCKET, SO_RCVTIMEO, &deadline, sizeof(deadline)), 0);
    return {Socket{fds[0]}, Socket{fds[1]}};
}

void send_all(const Socket& sock, std::string_view bytes) {
    EXPECT_EQ(::send(sock.get(), bytes.data(), bytes.size(), 0), static_cast<ssize_t>(bytes.size()));
}

// One whole request and then a half close, so the connection ends the moment it is answered and gives its worker back.
void ask(const Socket& client) {
    send_all(client, get_root);
    EXPECT_EQ(::shutdown(client.get(), SHUT_WR), 0);
}

// Everything the pool sent, to end of stream. Empty means the connection was closed without an answer.
std::string answered(const Socket& client) {
    std::string received;
    std::array<char, 4096> buf{};
    while (true) {
        const ssize_t got = ::recv(client.get(), buf.data(), buf.size(), 0);
        if (got <= 0) {
            return received;
        }
        received.append(buf.data(), static_cast<std::size_t>(got));
    }
}

std::string_view status_line(std::string_view response) {
    return response.substr(0, response.find("\r\n"));
}

std::shared_ptr<const Pipeline> routing(Handler handler) {
    auto pipeline = std::make_shared<Pipeline>();
    pipeline->add(Method::Get, "/", std::move(handler));
    return pipeline;
}

Handler echo() {
    return [](const Request& request) { return text_response(200, "you asked for " + request.target + "\n"); };
}

// Polls until something is true, with a ceiling, so a test that would otherwise spin for ever goes red instead.
template <typename Predicate>
bool became_true(Predicate holds) {
    for (int waited = 0; waited < 1000 && !holds(); ++waited) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return holds();
}

// Holds every handler that enters until a test lets go, and counts how many are inside. A worker inside a handler is a
// worker that cannot take the next connection, which is how a bound on workers is watched from outside the pool.
class Gate {
public:
    Handler handler() {
        return [this](const Request&) {
            enter();
            return text_response(200, "held\n");
        };
    }

    // Counted without a lock so a test can wait on this and then go on to block on a socket. Holding a mutex across
    // that would be a real hazard, and the analyser reads an inlined one as still held whatever the code does after.
    bool wait_until_inside(std::size_t count) {
        return became_true([this, count] { return inside_ >= count; });
    }

    [[nodiscard]] std::size_t inside() const noexcept {
        return inside_;
    }

    void release() {
        {
            const std::scoped_lock lock(mutex_);
            open_ = true;
        }
        opened_.notify_all();
    }

private:
    void enter() {
        ++inside_;
        std::unique_lock<std::mutex> lock(mutex_);
        static_cast<void>(opened_.wait_for(lock, std::chrono::seconds(5), [this] { return open_; }));
    }

    std::mutex mutex_;
    std::condition_variable opened_;
    std::atomic<std::size_t> inside_{0};
    bool open_ = false;
};

TEST(ConnectionPool, ServesAConnectionHandedToIt) {
    auto pair = connected_pair();
    ask(pair.first);

    // Read before the pool goes: stopping drops whatever is still queued, and a worker may not have reached this yet.
    ConnectionPool pool{routing(echo()), roomy, brief};
    pool.submit(std::move(pair.second));

    EXPECT_EQ(status_line(answered(pair.first)), "HTTP/1.1 200 OK");
}

// The bound is on workers rather than on connections: three arrive together, and only as many as there are workers can
// be inside a handler at once. The third waits its turn instead of getting a thread of its own.
TEST(ConnectionPool, ServesNoMoreAtOnceThanItHasWorkers) {
    Gate gate;
    std::vector<std::pair<Socket, Socket>> pairs;
    pairs.reserve(3);
    for (int i = 0; i < 3; ++i) {
        pairs.push_back(connected_pair());
    }

    {
        ConnectionPool pool{routing(gate.handler()), PoolLimits{2, 8, std::chrono::seconds(5)}, brief};
        for (auto& pair : pairs) {
            ask(pair.first);
            pool.submit(std::move(pair.second));
        }

        ASSERT_TRUE(gate.wait_until_inside(2));

        // Long enough for a third worker to show up if there were one. Nothing else releases them, so the count can
        // only rise.
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        EXPECT_EQ(gate.inside(), 2U);

        gate.release();
    }
}

// A queue with no room left closes the connection instead of holding it, so that client hears at once rather than
// waiting behind everything already in front of it.
TEST(ConnectionPool, AnswersAConnectionTheQueueHasNoRoomForWithFiveOhThree) {
    Gate gate;
    auto held = connected_pair();
    auto queued = connected_pair();
    auto refused = connected_pair();

    {
        ConnectionPool pool{routing(gate.handler()), PoolLimits{1, 1, std::chrono::seconds(5)}, brief};

        // The one worker takes this one straight off the queue, which leaves the queue empty again.
        ask(held.first);
        pool.submit(std::move(held.second));
        ASSERT_TRUE(gate.wait_until_inside(1));

        ask(queued.first);
        pool.submit(std::move(queued.second));

        // Nothing sent on this one. The refusal arrives either way, but a peer closed with bytes it never read resets
        // the connection, and a clean end of stream is the clearer second thing to assert on.
        pool.submit(std::move(refused.second));

        EXPECT_EQ(status_line(answered(refused.first)), "HTTP/1.1 503 Service Unavailable");

        // End of stream rather than the client's own deadline: zero says closed, a timeout would say still open.
        char byte = 0;
        EXPECT_EQ(::recv(refused.first.get(), &byte, 1, 0), ssize_t{0}) << "the refused connection is still open";

        gate.release();
    }
}

// A connection that sat in the queue longer than anyone is likely to still be waiting for is dropped rather than
// served: the worker that finally reaches it would otherwise spend itself on a client that has gone.
TEST(ConnectionPool, AnswersAConnectionThatWaitedTooLongWithFiveOhThree) {
    Gate gate;
    auto held = connected_pair();
    auto stale = connected_pair();

    {
        ConnectionPool pool{routing(gate.handler()), PoolLimits{1, 8, std::chrono::milliseconds(50)}, brief};

        ask(held.first);
        pool.submit(std::move(held.second));
        ASSERT_TRUE(gate.wait_until_inside(1));

        ask(stale.first);
        pool.submit(std::move(stale.second));
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        gate.release();

        // Read while the pool is still up, so what ends this is the queue deadline and not the pool shutting down. The
        // handler never ran, which is what tells a 503 here apart from a response the route produced.
        EXPECT_EQ(status_line(answered(stale.first)), "HTTP/1.1 503 Service Unavailable");
    }

    EXPECT_EQ(status_line(answered(held.first)), "HTTP/1.1 200 OK");
}

// Stopping drops what is still queued rather than working through it, so a connection that never reached a worker is
// closed without an answer.
TEST(ConnectionPool, ClosesQueuedConnectionsItNeverReached) {
    Gate gate;
    auto held = connected_pair();
    auto never = connected_pair();

    auto pool =
        std::make_unique<ConnectionPool>(routing(gate.handler()), PoolLimits{1, 8, std::chrono::seconds(5)}, brief);

    ask(held.first);
    pool->submit(std::move(held.second));
    ASSERT_TRUE(gate.wait_until_inside(1));

    ask(never.first);
    pool->submit(std::move(never.second));

    // Destroyed from another thread, because the destructor sits in join until the handler lets go, and the handler is
    // let go only once stopping has been recorded.
    std::thread stopping([&pool] { pool.reset(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    gate.release();
    stopping.join();

    EXPECT_TRUE(answered(never.first).empty()) << "a queued connection was served after the pool had stopped";
}

// The destructor joins rather than detaches, so a handler still running when the pool goes out of scope has finished by
// the time the next line runs.
TEST(ConnectionPool, FinishesWhatItStartedBeforeItIsGone) {
    std::atomic<bool> started{false};
    std::atomic<bool> finished{false};
    auto pair = connected_pair();
    ask(pair.first);

    {
        ConnectionPool pool{routing([&started, &finished](const Request&) {
                                started = true;
                                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                                finished = true;
                                return text_response(200, "done\n");
                            }),
                            roomy, brief};
        pool.submit(std::move(pair.second));

        // Leaving before the handler has begun would prove nothing: a worker still waiting for work stops at once.
        ASSERT_TRUE(became_true([&started] { return started.load(); }));
    }

    EXPECT_TRUE(finished) << "the pool let go of a worker that was still running";
}

// The deadlines handed to the pool are the ones each connection gets. Without them every connection would carry the
// half minute a Connection defaults to, and a client that went quiet mid-request would hold its worker for all of it.
TEST(ConnectionPool, ServesConnectionsWithTheDeadlinesItWasGiven) {
    auto pair = connected_pair();
    send_all(pair.first, "GET /par");

    const auto started = std::chrono::steady_clock::now();
    ConnectionPool pool{routing(echo()), roomy, brief};
    pool.submit(std::move(pair.second));

    const std::string response = answered(pair.first);
    const auto took = std::chrono::steady_clock::now() - started;

    EXPECT_EQ(status_line(response), "HTTP/1.1 408 Request Timeout");
    EXPECT_LT(took, std::chrono::seconds(2));
}

// RFC 9112 §9.6: a response that ends the connection says so, or a client cannot tell a deliberate end from a reply
// that was cut short.
TEST(ConnectionPool, SaysTheConnectionIsClosingOnARefusal) {
    Gate gate;
    auto held = connected_pair();
    auto queued = connected_pair();
    auto refused = connected_pair();

    {
        ConnectionPool pool{routing(gate.handler()), PoolLimits{1, 1, std::chrono::seconds(5)}, brief};
        ask(held.first);
        pool.submit(std::move(held.second));
        ASSERT_TRUE(gate.wait_until_inside(1));
        ask(queued.first);
        pool.submit(std::move(queued.second));
        pool.submit(std::move(refused.second));

        const std::string response = answered(refused.first);

        EXPECT_NE(response.find("\r\nconnection: close\r\n"), std::string::npos) << response;
        EXPECT_NE(response.find("503 Service Unavailable\n"), std::string::npos) << response;

        gate.release();
    }
}

// The refusal is written on the accept thread, so it must never wait for the client to read. A socket with no room left
// is the case that would hang an accept loop, and refusing has to cost it nothing.
TEST(ConnectionPool, DoesNotWaitOnAClientThatIsNotReadingWhenItRefuses) {
    Gate gate;
    auto held = connected_pair();
    auto queued = connected_pair();
    auto refused = connected_pair();

    {
        ConnectionPool pool{routing(gate.handler()), PoolLimits{1, 1, std::chrono::seconds(5)}, brief};
        ask(held.first);
        pool.submit(std::move(held.second));
        ASSERT_TRUE(gate.wait_until_inside(1));
        ask(queued.first);
        pool.submit(std::move(queued.second));

        // The far end of this one never reads, and it is stuffed full before it is handed over.
        EXPECT_GT(fill(refused.second), 0U);

        const auto started = std::chrono::steady_clock::now();
        pool.submit(std::move(refused.second));
        const auto took = std::chrono::steady_clock::now() - started;

        EXPECT_LT(took, std::chrono::milliseconds(250)) << "submit waited on a client that was not reading";

        gate.release();
    }
}

// The failure a test can aim at the library rather than at the caller: the reader's buffer growing past the armed size
// while it takes in a body. A handler that throws is a 500 long before the worker would see it, so nothing above the
// reader can stand in for this. What the catch is for is the second half: the worker is still there afterwards.
TEST(ConnectionPool, KeepsTheWorkerAfterAFailureInsideAConnection) {
#ifdef CARAFE_TEST_WITHOUT_ALLOCATION_FAILURE
    GTEST_SKIP() << "no allocation failure can be arranged in a sanitizer build";
#else
    // Built before arming, so the test's own large allocation is not the one that fails.
    const std::string body(512UL * 1024, 'x');
    const std::string request =
        "GET / HTTP/1.1\r\nHost: example.test\r\nContent-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;

    auto failing = connected_pair();
    auto served = connected_pair();

    {
        ConnectionPool pool{routing(echo()), roomy, brief};

        fail_next_allocation_of_at_least(256UL * 1024);
        pool.submit(std::move(failing.second));
        send_until_closed(failing.first, request);

        EXPECT_TRUE(answered(failing.first).empty()) << "the connection whose read failed was answered anyway";
        stop_failing_allocations();

        ask(served.first);
        pool.submit(std::move(served.second));

        EXPECT_EQ(status_line(answered(served.first)), "HTTP/1.1 200 OK") << "the worker did not outlive the failure";
    }
#endif
}

TEST(ConnectionPool, AnswersAConnectionTheQueueCannotGrowForWithFiveOhThree) {
#ifdef CARAFE_TEST_WITHOUT_ALLOCATION_FAILURE
    GTEST_SKIP() << "no allocation failure can be arranged in a sanitizer build";
#else
    // No workers, so the test thread is the only one allocating and nothing drains the queue while it fills: the
    // allocation that fails is then one queueing asked for and cannot be some worker's. The pool takes the limits it
    // is given; App is where a pool that would serve nobody is refused.
    const PoolLimits unserved{0, 64, std::chrono::seconds(5)};

    // Built before anything is armed, since building them allocates too. One per queue slot because a deque grows a
    // node every so many entries rather than on every push, and which push that falls on is the container's business
    // rather than this test's.
    std::vector<std::pair<Socket, Socket>> clients;
    clients.reserve(unserved.queued);
    for (std::size_t i = 0; i < unserved.queued; i++) {
        clients.push_back(connected_pair());
    }

    ConnectionPool pool{routing(echo()), unserved, brief};

    std::size_t refused = clients.size();
    for (std::size_t i = 0; i < clients.size(); i++) {
        fail_next_allocation_of_at_least(1);
        pool.submit(std::move(clients[i].second));

        const bool failed = an_allocation_failed();
        stop_failing_allocations();
        if (failed) {
            refused = i;
            break;
        }
    }

    // Reaching here at all is half the assertion: an exception out of submit is an exception out of the accept loop.
    ASSERT_LT(refused, clients.size()) << "queueing never allocated, so nothing here reached the catch";
    EXPECT_EQ(status_line(answered(clients[refused].first)), "HTTP/1.1 503 Service Unavailable");
#endif
}

}  // namespace
