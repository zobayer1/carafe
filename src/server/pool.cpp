#include "server/pool.hpp"

#include <carafe/config.hpp>
#include <carafe/http/response.hpp>

#include "net/socket.hpp"
#include "server/connection.hpp"
#include "server/pipeline.hpp"
#include "server/serve.hpp"

#include <chrono>
#include <cstddef>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <system_error>
#include <thread>
#include <utility>

namespace carafe::server {

namespace {

// The one response the pool ever writes, serialized at construction. RFC 9112 §9.6: a response that ends the
// connection has to say so, or a client cannot tell a deliberate end from a reply cut short.
[[nodiscard]] std::string refusal_bytes() {
    http::Response response = http::status_response(503);
    response.headers.add({"connection", "close"});
    return response.serialize();
}

}  // namespace

ConnectionPool::ConnectionPool(std::shared_ptr<const Pipeline> pipeline, PoolLimits limits, Deadlines deadlines)
    : pipeline_(std::move(pipeline)), limits_(limits), deadlines_(deadlines), refusal_(refusal_bytes()) {
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
    try {
        // The lock ends where the try does, and both end where queueing does. The refusal below is a send, and
        // the accept thread holding the pool's one mutex across a send is contention every worker pays for.
        const std::unique_lock<std::mutex> lock(mutex_);
        if (!stopping_ && queue_.size() < limits_.queued) {
            // The node is made before the connection goes into it. A push that fails takes only the node, so the
            // socket is still ours and the client still learns why.
            queue_.push_back({net::Socket{-1}, std::chrono::steady_clock::now()});
            queue_.back().client = std::move(client);
            ready_.notify_one();
            return;
        }
    } catch (...) {
        // The queue could not grow. Answering costs nothing that could fail again: refusal_ was serialized at
        // construction and write_now neither allocates nor waits.
        static_cast<void>(client.write_now(refusal_));
        return;
    }

    // Refused. Say so if the socket takes it now, and close either way: the parameter still owns it.
    static_cast<void>(client.write_now(refusal_));
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
            // The refusal a full queue gets, and best effort for the same reason: this client waited past its own
            // deadline, so a worker blocking to reach it would spend that time on someone most likely gone.
            static_cast<void>(job->client.write_now(refusal_));
            continue;
        }

        try {
            Connection conn{std::move(job->client), deadlines_};
            serve_connection(conn, *pipeline_);
        } catch (...) {
            // Last resort, and what reaches here is the library failing rather than the caller: a handler that throws
            // is already a 500. There is nothing to answer with, since answering allocates too, so this connection is
            // dropped and the worker takes the next. Keeping the worker is the point: an exception leaving it takes
            // the process with it.
            continue;
        }
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
