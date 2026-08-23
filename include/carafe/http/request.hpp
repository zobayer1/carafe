#pragma once

#include <carafe/http/headers.hpp>

#include <string>
#include <string_view>

namespace carafe::http {

enum class Method { Get, Head, Post, Put, Delete, Connect, Options, Trace, Patch };

enum class Version { Http10, Http11 };

struct Request {
    Method method{};
    std::string target;
    Version version{};
    Headers headers;
};

struct RequestLine {
    Method method{};
    std::string target;
    Version version{};
};

[[nodiscard]] std::string_view method_name(Method method) noexcept;

}  // namespace carafe::http
