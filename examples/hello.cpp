// The smallest thing that proves the socket, the parser, the router and the
// responder are joined up, plus enough routes to drive a body through by hand.
// docs/examples.md walks through what each one answers.
//
// Connections are served one at a time, to completion, and a persistent one is
// held until the client hangs up, so a browser holding one open locks everyone
// else out. That is what the concurrency milestone is for.

#include <carafe/app.hpp>
#include <carafe/http/request.hpp>
#include <carafe/http/response.hpp>
#include <carafe/version.hpp>

#include <cstdint>
#include <iostream>
#include <string>
#include <utility>

using carafe::http::Method;
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

    app.get("/hello/<name>", [](const Request& request) {
        std::string body = "hello, ";
        body += request.params.get("name").value_or("world");
        body += "!\n";
        return text_response(200, std::move(body));
    });

    // Hands the bytes straight back, so what comes out is proof of what went in.
    app.post("/echo", [](const Request& request) { return text_response(200, request.body); });

    // The count rather than the bytes, for a body too big to want echoed back.
    app.post("/size", [](const Request& request) {
        return text_response(200, std::to_string(request.body.size()) + " bytes\n");
    });

    // A capture and a body in one request: neither reaches the handler by the
    // same route, and this is where they would collide if they did.
    app.put("/store/<key>", [](const Request& request) {
        std::string body{request.params.get("key").value_or("?")};
        body += " = ";
        body += request.body;
        body += '\n';
        return text_response(200, std::move(body));
    });

    // A second body-carrying verb, to show framing does not consult the method:
    // the same Content-Length rules answer this and the POST above.
    app.patch("/store/<key>", [](const Request& request) {
        std::string body{request.params.get("key").value_or("?")};
        body += " += ";
        body += request.body;
        body += '\n';
        return text_response(200, std::move(body));
    });

    // No body of its own, and one more verb on a path that already has several,
    // which is what puts a real list in the allow: of a 405.
    app.del("/store/<key>", [](const Request& request) {
        std::string body{request.params.get("key").value_or("?")};
        body += " deleted\n";
        return text_response(200, std::move(body));
    });

    // route() reaches a verb with no named helper. It answers false for the two
    // that cannot be registered honestly, so the result is worth acting on even
    // when the method is a literal.
    const bool registered = app.route(Method::Options, "/store/<key>", [](const Request&) {
        carafe::http::Response response = text_response(200, "");
        response.headers.add({"allow", "PUT, PATCH, DELETE, OPTIONS"});
        return response;
    });
    if (!registered) {
        std::cerr << "OPTIONS could not be registered\n";
        return 1;
    }

    // Flushed, because run() blocks immediately afterwards and a piped stdout
    // would otherwise hold this until the process ends.
    std::cout << "carafe " << carafe::version() << " serving on http://localhost:" << port
              << "\ntry:  curl -i http://localhost:" << port << '/'
              << "\n      curl -i http://localhost:" << port << "/hello/world"
              << "\n      curl -i --data 'hi' http://localhost:" << port << "/echo"
              << "\n      curl -i -X PUT --data 'v' http://localhost:" << port << "/store/k"
              << "\n      see docs/examples.md for the rest\n"
              << std::flush;

    if (!app.run(port)) {
        std::cerr << "could not serve on port " << port << '\n';
        return 1;
    }
    return 0;
}
