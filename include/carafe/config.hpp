#pragma once

#include <chrono>
#include <cstddef>

namespace carafe {

// How much work the server holds at once. `workers` bounds what is being served, `queued` absorbs the arrivals that
// land while they are busy, and a connection waiting longer than `queue_wait` is dropped rather than served.
struct PoolLimits {
    std::size_t workers = 64;
    std::size_t queued = 512;
    std::chrono::milliseconds queue_wait{std::chrono::seconds(5)};
};

// How long a connection may wait, in two parts. Separate because waiting for a request to begin should be generous to a
// client with nothing to say yet, and waiting for one to finish should not: the second is the only one a client can
// renew by sending anything at all.
struct Deadlines {
    std::chrono::milliseconds idle{std::chrono::seconds(30)};
    std::chrono::milliseconds request{std::chrono::seconds(30)};
};

}  // namespace carafe
