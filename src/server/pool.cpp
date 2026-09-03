#include "server/pool.hpp"

#include "net/socket.hpp"
#include "server/connection.hpp"
#include "server/router.hpp"
#include "server/serve.hpp"

#include <chrono>
#include <cstddef>
#include <memory>
#include <mutex>
#include <optional>
#include <system_error>
#include <thread>
#include <utility>

namespace carafe::server {

ConnectionPool::ConnectionPool(std::shared_ptr<const Router> router, PoolLimits limits, Deadlines deadlines)
    : router_(std::move(router)), limits_(limits), deadlines_(deadlines) {
    for (std::size_t i = 0; i < limits_.workers; i++) {
        try {
            workers_.emplace_back(&ConnectionPool::work, this);
        } catch (const std::system_error&) {
            // Out of threads. A smaller pool than asked for still serves, and a constructor that throws has no
            // destructor to join the workers already running.
            break;
        }
    }
}

ConnectionPool::~ConnectionPool() {
    stop();
}

void ConnectionPool::submit(net::Socket client) {
    const std::unique_lock<std::mutex> lock(mutex_);
    if (stopping_ || queue_.size() >= limits_.queued) {
        // The parameter holds the socket by value, so returning closes it.
        return;
    }
    queue_.push_back({std::move(client), std::chrono::steady_clock::now()});
    ready_.notify_one();
}

void ConnectionPool::work() {
    while (true) {
        std::optional<Waiting> job;
        {
            std::unique_lock lock{mutex_};
            ready_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
            if (stopping_) {
                return;
            }
            job.emplace(std::move(queue_.front()));
            queue_.pop_front();
        }

        if (std::chrono::steady_clock::now() - job->queued_at > limits_.queue_wait) {
            continue;
        }

        Connection conn{std::move(job->client), deadlines_};
        serve_connection(conn, *router_);
    }
}

void ConnectionPool::stop() {
    // The lock is released before the joins below: a worker woken by the notify needs it back to see the flag, and
    // holding it across a join would deadlock against the worker being joined.
    {
        const std::unique_lock<std::mutex> lock(mutex_);
        stopping_ = true;
    }
    ready_.notify_all();
    for (auto& worker : workers_) {
        if (worker.joinable()) {
            worker.join();
        }
    }
}

}  // namespace carafe::server
