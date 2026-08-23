#include <carafe/http/request.hpp>

namespace carafe::http {

std::string_view method_name(Method method) noexcept {
    switch (method) {
        case Method::Get:
            return "GET";
        case Method::Head:
            return "HEAD";
        case Method::Post:
            return "POST";
        case Method::Put:
            return "PUT";
        case Method::Delete:
            return "DELETE";
        case Method::Connect:
            return "CONNECT";
        case Method::Options:
            return "OPTIONS";
        case Method::Trace:
            return "TRACE";
        case Method::Patch:
            return "PATCH";
    }
    return "";
}

}  // namespace carafe::http
