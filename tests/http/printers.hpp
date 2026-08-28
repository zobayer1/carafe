#pragma once

#include <carafe/http/request.hpp>

#include "http/line_reader.hpp"
#include "http/request_parser.hpp"
#include "http/request_reader.hpp"

#include <ostream>

// gtest finds these by ADL, so they must sit in carafe::http rather than in a test file's anonymous namespace. `inline`
// is what lets more than one test translation unit include them without a duplicate-symbol link error.
namespace carafe::http {

inline std::ostream& operator<<(std::ostream& os, Method method) {
    switch (method) {
        case Method::Get:
            return os << "Get";
        case Method::Head:
            return os << "Head";
        case Method::Post:
            return os << "Post";
        case Method::Put:
            return os << "Put";
        case Method::Delete:
            return os << "Delete";
        case Method::Connect:
            return os << "Connect";
        case Method::Options:
            return os << "Options";
        case Method::Trace:
            return os << "Trace";
        case Method::Patch:
            return os << "Patch";
    }
    return os << "Method(" << static_cast<int>(method) << ")";
}

inline std::ostream& operator<<(std::ostream& os, Version version) {
    switch (version) {
        case Version::Http10:
            return os << "HTTP/1.0";
        case Version::Http11:
            return os << "HTTP/1.1";
    }
    return os << "Version(" << static_cast<int>(version) << ")";
}

inline std::ostream& operator<<(std::ostream& os, ParseError error) {
    switch (error) {
        case ParseError::None:
            return os << "None";
        case ParseError::Malformed:
            return os << "Malformed";
        case ParseError::UnknownMethod:
            return os << "UnknownMethod";
        case ParseError::UnsupportedVersion:
            return os << "UnsupportedVersion";
    }
    return os << "ParseError(" << static_cast<int>(error) << ")";
}

inline std::ostream& operator<<(std::ostream& os, LineError error) {
    switch (error) {
        case LineError::None:
            return os << "None";
        case LineError::LineTooLong:
            return os << "LineTooLong";
    }
    return os << "LineError(" << static_cast<int>(error) << ")";
}

inline std::ostream& operator<<(std::ostream& os, RequestError error) {
    switch (error) {
        case RequestError::None:
            return os << "None";
        case RequestError::Malformed:
            return os << "Malformed";
        case RequestError::UnknownMethod:
            return os << "UnknownMethod";
        case RequestError::UnsupportedVersion:
            return os << "UnsupportedVersion";
        case RequestError::RequestLineTooLong:
            return os << "RequestLineTooLong";
        case RequestError::HeaderTooLong:
            return os << "HeaderTooLong";
        case RequestError::TooManyHeaders:
            return os << "TooManyHeaders";
        case RequestError::HeadTooLarge:
            return os << "HeadTooLarge";
        case RequestError::BodyTooLarge:
            return os << "BodyTooLarge";
        case RequestError::UnsupportedTransferEncoding:
            return os << "UnsupportedTransferEncoding";
    }
    return os << "RequestError(" << static_cast<int>(error) << ")";
}

}  // namespace carafe::http
