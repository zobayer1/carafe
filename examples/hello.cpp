// The smallest thing that proves the socket, the parser and the responder are
// joined up: a server that answers every request the same way. Routes and
// handlers are the next milestone.
//
// Connections are served one at a time, to completion, and kept alive until the
// client hangs up -- so a browser holding a connection open locks everyone else
// out. That is what the concurrency milestone is for.

#include <carafe/app.hpp>
#include <carafe/version.hpp>

#include <cstdint>
#include <iostream>

int main() {
    constexpr std::uint16_t port = 8080;

    // Flushed, because run() blocks immediately afterwards and a piped stdout
    // would otherwise hold this until the process ends.
    std::cout << "carafe " << carafe::version() << " starting on http://localhost:" << port
              << "\ntry:  curl -i http://localhost:" << port << "/hello\n"
              << std::flush;

    carafe::App app;
    if (!app.run(port)) {
        std::cerr << "could not serve on port " << port << '\n';
        return 1;
    }
    return 0;
}
