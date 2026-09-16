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
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <gtest/gtest.h>
#include <sys/socket.h>
#include <sys/time.h>

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

}  // namespace
