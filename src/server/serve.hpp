#pragma once

#include "server/connection.hpp"

namespace carafe::server {

// Answers every request on this connection until the client finishes, a head
// cannot be parsed, or a write fails.
void serve_connection(Connection& conn);

}  // namespace carafe::server
