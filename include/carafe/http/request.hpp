#pragma once

#include <string>

namespace carafe::http {

enum class Method { Get, Head, Post, Put, Delete, Connect, Options, Trace, Patch };

enum class Version { Http10, Http11 };

struct RequestLine {
    Method method{};
    std::string target;
    Version version{};
};

}  // namespace carafe::http
