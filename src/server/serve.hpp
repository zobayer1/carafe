#pragma once

#include <carafe/config.hpp>

#include "net/listener.hpp"
#include "server/connection.hpp"
#include "server/pipeline.hpp"

#include <memory>

namespace carafe::server {

// Answers every request on this connection until the client finishes, a head cannot be parsed, or a write fails.
void serve_connection(Connection& conn, const Pipeline& pipeline);

// Serves every connection this listener accepts, on a bounded pool of threads. Returns only when accepting fails for a
// reason retrying would not fix.
void serve_forever(net::Listener& listener, const std::shared_ptr<const Pipeline>& pipeline, PoolLimits limits = {},
                   Deadlines deadlines = {});

}  // namespace carafe::server
