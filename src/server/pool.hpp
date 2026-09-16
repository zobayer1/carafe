#pragma once

#include <carafe/config.hpp>

#include "net/socket.hpp"
#include "server/connection.hpp"
#include "server/pipeline.hpp"

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace carafe::server {

// A fixed set of threads serving accepted connections, and a bounded queue of those waiting for one.
//
// One thread serves a whole connection rather than one request, so a keep-alive client holds its worker until it goes
// away. A full pool therefore makes new clients wait for an existing one to end, which is what `queue_wait` bounds.
class ConnectionPool {
public:
    explicit ConnectionPool(std::shared_ptr<const Pipeline> pipeline, PoolLimits limits = {}, Deadlines deadlines = {});

    // Stops the workers and joins them. Queued connections are closed unserved, and ones being served are finished.
    ~ConnectionPool();

    ConnectionPool(const ConnectionPool&) = delete;
    ConnectionPool& operator=(const ConnectionPool&) = delete;

    // Every worker holds `this`, so a moved pool would leave them running against an object that has gone.
    ConnectionPool(ConnectionPool&&) = delete;
    ConnectionPool& operator=(ConnectionPool&&) = delete;

    // Takes one accepted connection. A queue with no room for it is answered with a 503 where the socket takes one
    // without waiting, and closed either way: an accept loop that waited, for room or for a write, would become the
    // queue itself.
    void submit(net::Socket client);

private:
    // A connection and when it joined the queue, so a worker can see how long it waited before it starts.
    struct Waiting {
        net::Socket client;
        std::chrono::steady_clock::time_point queued_at;
    };

    void work();
    void stop();

    std::shared_ptr<const Pipeline> pipeline_;
    PoolLimits limits_;
    Deadlines deadlines_;

    std::string refusal_;

    std::mutex mutex_;
    std::condition_variable ready_;
    std::deque<Waiting> queue_;
    bool stopping_ = false;

    // Last, so every member a worker touches exists before any worker starts.
    std::vector<std::thread> workers_;
};

}  // namespace carafe::server
