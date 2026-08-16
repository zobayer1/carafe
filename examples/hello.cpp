// The smallest thing that proves the socket, the parser, the router and the
// responder are joined up: two routes, and a 404 for everything else.
//
// Connections are served one at a time, to completion, and kept alive until the
// client hangs up -- so a browser holding a connection open locks everyone else
// out. That is what the concurrency milestone is for.

#include <carafe/app.hpp>
#include <carafe/http/request.hpp>
#include <carafe/http/response.hpp>
#include <carafe/version.hpp>

#include <cstdint>
#include <iostream>
#include <string>
#include <utility>

using carafe::http::Request;
using carafe::http::text_response;

int main() {
    constexpr std::uint16_t port = 8080;

    carafe::App app;

    app.get("/", [](const Request&) {
        std::string body = "carafe ";
        body += carafe::version();
        body += "\ntry /hello\n";
        return text_response(200, std::move(body));
    });

    app.get("/hello", [](const Request& request) {
        std::string body = "you asked for ";
        body += request.target;
        body += '\n';
        return text_response(200, std::move(body));
    });

    // Flushed, because run() blocks immediately afterwards and a piped stdout
    // would otherwise hold this until the process ends.
    std::cout << "carafe " << carafe::version() << " serving on http://localhost:" << port
              << "\ntry:  curl -i http://localhost:" << port << '/'
              << "\n      curl -i http://localhost:" << port << "/hello"
              << "\n      curl -i http://localhost:" << port << "/missing\n"
              << std::flush;

    if (!app.run(port)) {
        std::cerr << "could not serve on port " << port << '\n';
        return 1;
    }
    return 0;
}
