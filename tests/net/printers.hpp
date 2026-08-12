#pragma once

#include "net/listener.hpp"

#include <ostream>

namespace carafe::net {

inline std::ostream& operator<<(std::ostream& os, ListenError error) {
    switch (error) {
        case ListenError::None:
            return os << "None";
        case ListenError::SocketFailed:
            return os << "SocketFailed";
        case ListenError::BindFailed:
            return os << "BindFailed";
        case ListenError::AddressFailed:
            return os << "AddressFailed";
        case ListenError::ListenFailed:
            return os << "ListenFailed";
    }
    return os << "ListenError(" << static_cast<int>(error) << ")";
}

}  // namespace carafe::net
