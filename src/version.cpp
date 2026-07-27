#include <carafe/version.hpp>

namespace carafe {

std::string_view version() noexcept {
    return CARAFE_VERSION_STRING;
}

}  // namespace carafe
