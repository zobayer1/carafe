#pragma once

#include "server/connection.hpp"
#include "server/router.hpp"

namespace carafe::server {

// Answers every request on this connection until the client finishes, a head cannot be parsed, or a write fails.
void serve_connection(Connection& conn, const Router& router);

}  // namespace carafe::server
