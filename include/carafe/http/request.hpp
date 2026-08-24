#pragma once

#include <carafe/http/headers.hpp>

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace carafe::http {

enum class Method { Get, Head, Post, Put, Delete, Connect, Options, Trace, Patch };

enum class Version { Http10, Http11 };

struct PathParam {
    std::string name;
    std::string value;
};

struct Params {
    std::vector<PathParam> entries;

    // The first entry with this name, as Headers::get answers: a pattern may bind
    // the same name twice, and the first is the one that reads naturally.
    // The view is valid while entries is unmodified.
    [[nodiscard]] std::optional<std::string_view> get(std::string_view name) const noexcept;
};

struct Request {
    Method method{};
    std::string target;
    Version version{};
    Headers headers;

    // Bound by the Router after the head is parsed, not by the parser.
    // Empty for a static route, and for a request no route claimed.
    Params params;
};

struct RequestLine {
    Method method{};
    std::string target;
    Version version{};
};

[[nodiscard]] std::string_view method_name(Method method) noexcept;

}  // namespace carafe::http
