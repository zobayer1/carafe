#pragma once

#include <carafe/http/request.hpp>
#include <carafe/http/response.hpp>

#include <functional>

namespace carafe::http {

using Handler = std::function<Response(const Request&)>;

}  // namespace carafe::http
