#pragma once

#include "net/listener.hpp"
#include "server/connection.hpp"
#include "server/pool.hpp"
#include "server/router.hpp"

#include <memory>

namespace carafe::server {

// Answers every request on this connection until the client finishes, a head cannot be parsed, or a write fails.
void serve_connection(Connection& conn, const Router& router);

// Serves every connection this listener accepts, on a bounded pool of threads. Returns only when accepting fails for a
// reason retrying would not fix.
void serve_forever(net::Listener& listener, const std::shared_ptr<const Router>& router, PoolLimits limits = {});

}  // namespace carafe::server
