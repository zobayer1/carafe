#pragma once

#include "net/socket.hpp"
#include "server/connection.hpp"
#include "server/router.hpp"

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace carafe::server {

// How much work the server holds at once. `workers` bounds what is being served, `queued` absorbs the arrivals that
// land while they are busy, and a connection waiting longer than `queue_wait` is dropped rather than served.
struct PoolLimits {
    std::size_t workers = 64;
    std::size_t queued = 512;
    std::chrono::milliseconds queue_wait{std::chrono::seconds(5)};
};

// A fixed set of threads serving accepted connections, and a bounded queue of those waiting for one.
//
// One thread serves a whole connection rather than one request, so a keep-alive client holds its worker until it goes
// away. A full pool therefore makes new clients wait for an existing one to end, which is what `queue_wait` bounds.
class ConnectionPool {
public:
    ConnectionPool(std::shared_ptr<const Router> router, PoolLimits limits = {}, Deadlines deadlines = {});

    // Stops the workers and joins them. Queued connections are closed unserved, and ones being served are finished.
    ~ConnectionPool();

    ConnectionPool(const ConnectionPool&) = delete;
    ConnectionPool& operator=(const ConnectionPool&) = delete;

    // Takes one accepted connection, and closes it when the queue is full: an accept loop that waited for room would
    // become the queue itself.
    void submit(net::Socket client);

private:
    // A connection and when it joined the queue, so a worker can see how long it waited before it starts.
    struct Waiting {
        net::Socket client;
        std::chrono::steady_clock::time_point queued_at;
    };

    void work();
    void stop();

    std::shared_ptr<const Router> router_;
    PoolLimits limits_;
    Deadlines deadlines_;

    std::mutex mutex_;
    std::condition_variable ready_;
    std::deque<Waiting> queue_;
    bool stopping_ = false;

    // Last, so every member a worker touches exists before any worker starts.
    std::vector<std::thread> workers_;
};

}  // namespace carafe::server
