#include <carafe/http/request.hpp>

#include <optional>
#include <string_view>

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

std::optional<std::string_view> Params::get(std::string_view name) const noexcept {
    for (const PathParam& entry : entries) {
        if (entry.name == name) {
            return entry.value;
        }
    }
    return std::nullopt;
}

}  // namespace carafe::http
