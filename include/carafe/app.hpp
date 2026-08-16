#pragma once

#include <cstdint>

namespace carafe {

// Serves requests on `port`. Returns only on failure -- false if the port could
// not be bound, or if accepting stopped for a reason retrying would not fix.
class App {
public:
    [[nodiscard]] bool run(std::uint16_t port);
};

}  // namespace carafe
