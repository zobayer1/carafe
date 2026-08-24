#include "server/serve.hpp"

#include <carafe/http/handler.hpp>
#include <carafe/http/request.hpp>
#include <carafe/http/response.hpp>

#include "net/socket.hpp"
#include "server/connection.hpp"
#include "server/router.hpp"

#include <array>
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>

#include <gtest/gtest.h>
#include <sys/socket.h>
#include <sys/time.h>

namespace {

using carafe::http::Handler;
using carafe::http::Method;
using carafe::http::Request;
using carafe::http::text_response;
using carafe::net::Socket;
using carafe::server::Connection;
using carafe::server::Router;
using carafe::server::serve_connection;

// Handlers echo the target, so a 200 says which route answered rather than only
// that something did.
Handler echo() {
    return [](const Request& request) {
        return text_response(200, "you asked for " + request.target + "\n");
    };
}

Router routing(std::string_view path) {
    Router router;
    router.add(Method::Get, path, echo());
    return router;
}

std::pair<Socket, Socket> connected_pair() {
    std::array<int, 2> fds{-1, -1};
    EXPECT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds.data()), 0);
    return {Socket{fds[0]}, Socket{fds[1]}};
}

void send_all(const Socket& sock, std::string_view bytes) {
    while (!bytes.empty()) {
        const ssize_t sent = ::send(sock.get(), bytes.data(), bytes.size(), 0);
        ASSERT_NE(sent, -1);
        bytes.remove_prefix(static_cast<std::size_t>(sent));
    }
}

// Serves the connection, then closes the server end so the client can read to end
// of stream. That is what makes "the whole response" a well defined thing to
// assert on -- a live connection would otherwise just block waiting for more.
std::string serve_and_read(std::pair<Socket, Socket>& pair, const Router& router) {
    // The client half-closes first: it has said all it intends to. Without that a
    // keep-alive responder is still waiting for the next request, quite correctly,
    // and the test deadlocks against its own server.
    EXPECT_EQ(::shutdown(pair.first.get(), SHUT_WR), 0);

    {
        Connection conn{std::move(pair.second)};
        serve_connection(conn, router);
    }

    std::string received;
    std::array<char, 4096> buf{};
    while (true) {
        const ssize_t got = ::recv(pair.first.get(), buf.data(), buf.size(), 0);
        if (got <= 0) {
            return received;
        }
        received.append(buf.data(), static_cast<std::size_t>(got));
    }
}

std::string_view status_line(std::string_view response) {
    return response.substr(0, response.find("\r\n"));
}

std::string_view body_of(std::string_view response) {
    const auto blank = response.find("\r\n\r\n");
    return blank == std::string_view::npos ? std::string_view{} : response.substr(blank + 4);
}

// The content-length the response claims, which is only worth having because it
// can then be checked against the bytes that actually followed.
std::size_t declared_length(std::string_view response) {
    constexpr std::string_view field = "\r\ncontent-length: ";
    const auto at = response.find(field);
    EXPECT_NE(at, std::string_view::npos);
    if (at == std::string_view::npos) {
        return 0;
    }

    const auto start = at + field.size();
    const auto end = response.find("\r\n", start);
    return std::stoul(std::string{response.substr(start, end - start)});
}

constexpr std::string_view get_root = "GET / HTTP/1.1\r\nHost: example.test\r\n\r\n";

TEST(ServeConnection, AnswersAGetWithATwoHundred) {
    auto pair = connected_pair();
    send_all(pair.first, get_root);

    const std::string response = serve_and_read(pair, routing("/"));

    EXPECT_EQ(status_line(response), "HTTP/1.1 200 OK");
    EXPECT_EQ(declared_length(response), body_of(response).size());
    EXPECT_NE(body_of(response).find("you asked for /"), std::string_view::npos);
}

// The target comes off the wire, so seeing it back proves the head was parsed
// rather than a fixed string being echoed.
TEST(ServeConnection, PutsTheParsedTargetInTheBody) {
    auto pair = connected_pair();
    send_all(pair.first, "GET /a/deep/path HTTP/1.1\r\nHost: example.test\r\n\r\n");

    const std::string response = serve_and_read(pair, routing("/a/deep/path"));

    EXPECT_NE(body_of(response).find("/a/deep/path"), std::string_view::npos);
}

// The classic HEAD bug is a content-length of zero. It must be the length the
// matching GET would have sent, with none of those bytes actually following.
TEST(ServeConnection, AnswersAHeadWithTheSameLengthAndNoBody) {
    auto with_body = connected_pair();
    send_all(with_body.first, get_root);
    const std::string get = serve_and_read(with_body, routing("/"));

    auto without_body = connected_pair();
    send_all(without_body.first, "HEAD / HTTP/1.1\r\nHost: example.test\r\n\r\n");
    // The same GET route answers it: HEAD is routed by the fallback, not by a
    // route of its own.
    const std::string head = serve_and_read(without_body, routing("/"));

    EXPECT_EQ(status_line(head), "HTTP/1.1 200 OK");
    EXPECT_EQ(declared_length(head), declared_length(get));
    EXPECT_GT(declared_length(head), 0U);
    EXPECT_TRUE(body_of(head).empty());
}

// Answerable, so it is answered -- and then closed, because there is no way to
// find where the next request starts in a stream that failed to parse.
TEST(ServeConnection, AnswersAMalformedHeadWithFourHundredAndCloses) {
    auto pair = connected_pair();
    send_all(pair.first, "NOTAREQUEST\r\n\r\n");

    const std::string response = serve_and_read(pair, Router{});

    EXPECT_EQ(status_line(response), "HTTP/1.1 400 Bad Request");
    EXPECT_NE(response.find("connection: close\r\n"), std::string::npos);
}

TEST(ServeConnection, AnswersAnUnknownMethodWithFiveOhOne) {
    auto pair = connected_pair();
    send_all(pair.first, "FROB / HTTP/1.1\r\nHost: example.test\r\n\r\n");

    EXPECT_EQ(status_line(serve_and_read(pair, Router{})), "HTTP/1.1 501 Not Implemented");
}

TEST(ServeConnection, AnswersAnUnsupportedVersionWithFiveOhFive) {
    auto pair = connected_pair();
    send_all(pair.first, "GET / HTTP/2.0\r\nHost: example.test\r\n\r\n");

    EXPECT_EQ(status_line(serve_and_read(pair, Router{})),
              "HTTP/1.1 505 HTTP Version Not Supported");
}

// Over the 8192-byte line cap, which is a different failure from a malformed one
// and has to reach a different status.
TEST(ServeConnection, AnswersAnOverlongRequestLineWithFourFourteen) {
    auto pair = connected_pair();
    const std::string target(9000, 'x');
    send_all(pair.first, "GET /" + target + " HTTP/1.1\r\nHost: example.test\r\n\r\n");

    EXPECT_EQ(status_line(serve_and_read(pair, Router{})), "HTTP/1.1 414 URI Too Long");
}

// Past the 100-field cap. Three separate errors share this status, so the mapping
// has to fold them rather than name one.
TEST(ServeConnection, AnswersTooManyHeadersWithFourThirtyOne) {
    auto pair = connected_pair();
    std::string request = "GET / HTTP/1.1\r\nHost: example.test\r\n";
    for (int i = 0; i < 200; ++i) {
        request += "X-Filler-" + std::to_string(i) + ": v\r\n";
    }
    request += "\r\n";
    send_all(pair.first, request);

    EXPECT_EQ(status_line(serve_and_read(pair, Router{})),
              "HTTP/1.1 431 Request Header Fields Too Large");
}

// Keep-alive is the point of the loop: two heads in, two responses out, and no
// connection: close on either.
TEST(ServeConnection, AnswersTwoRequestsOnOneConnection) {
    auto pair = connected_pair();
    send_all(pair.first,
             "GET /one HTTP/1.1\r\nHost: a.test\r\n\r\n"
             "GET /two HTTP/1.1\r\nHost: b.test\r\n\r\n");

    Router router;
    router.add(Method::Get, "/one", echo());
    router.add(Method::Get, "/two", echo());

    const std::string response = serve_and_read(pair, router);

    EXPECT_NE(response.find("/one"), std::string::npos);
    EXPECT_NE(response.find("/two"), std::string::npos);
    EXPECT_EQ(response.find("connection: close"), std::string::npos);
    EXPECT_EQ(response.find("HTTP/1.1 200 OK"), 0U);
    EXPECT_NE(response.rfind("HTTP/1.1 200 OK"), 0U);
}

TEST(ServeConnection, AnswersAnUnregisteredPathWithFourOhFour) {
    auto pair = connected_pair();
    send_all(pair.first, "GET /missing HTTP/1.1\r\nHost: example.test\r\n\r\n");

    const std::string response = serve_and_read(pair, routing("/here"));

    EXPECT_EQ(status_line(response), "HTTP/1.1 404 Not Found");
    // The body names the status too, so a person reading a terminal gets more
    // than a blank page.
    EXPECT_EQ(body_of(response), "404 Not Found\n");
}

// The distinction the router's path_matched flag exists for: the resource is
// there, the verb is not, and the client learns something from the difference.
TEST(ServeConnection, AnswersAKnownPathUnderAnotherMethodWithFourOhFive) {
    auto pair = connected_pair();
    send_all(pair.first, "GET /submit HTTP/1.1\r\nHost: example.test\r\n\r\n");

    Router router;
    router.add(Method::Post, "/submit", echo());

    const std::string response = serve_and_read(pair, router);

    EXPECT_EQ(status_line(response), "HTTP/1.1 405 Method Not Allowed");
    // RFC 9110 makes this a MUST: a 405 that does not say what would have worked
    // leaves the client guessing one verb at a time.
    EXPECT_NE(response.find("allow: POST\r\n"), std::string::npos);
}

// Every method the path serves, in one field, comma-separated -- and HEAD among
// them, because the GET route really does answer it.
TEST(ServeConnection, NamesEveryMethodThePathServesOnAFourOhFive) {
    auto pair = connected_pair();
    send_all(pair.first, "PUT /thing HTTP/1.1\r\nHost: example.test\r\n\r\n");

    Router router;
    router.add(Method::Get, "/thing", echo());
    router.add(Method::Post, "/thing", echo());
    router.add(Method::Delete, "/thing", echo());

    const std::string response = serve_and_read(pair, router);

    EXPECT_EQ(status_line(response), "HTTP/1.1 405 Method Not Allowed");
    EXPECT_NE(response.find("allow: GET, HEAD, POST, DELETE\r\n"), std::string::npos);
}

// The field names methods for a resource, so a path that has none has nothing to
// say -- an empty Allow would claim the resource exists and serves nothing.
TEST(ServeConnection, SendsNoAllowWithAFourOhFour) {
    auto pair = connected_pair();
    send_all(pair.first, "GET /missing HTTP/1.1\r\nHost: example.test\r\n\r\n");

    const std::string response = serve_and_read(pair, routing("/here"));

    EXPECT_EQ(status_line(response), "HTTP/1.1 404 Not Found");
    EXPECT_EQ(response.find("allow:"), std::string::npos);
}

// HEAD reaches the 405 like any other method when no GET route implies it, and
// the header still arrives: it is the head that carries the answer.
TEST(ServeConnection, SendsAllowOnAFourOhFiveToHeadWithoutABody) {
    auto pair = connected_pair();
    send_all(pair.first, "HEAD /submit HTTP/1.1\r\nHost: example.test\r\n\r\n");

    Router router;
    router.add(Method::Post, "/submit", echo());

    const std::string response = serve_and_read(pair, router);

    EXPECT_EQ(status_line(response), "HTTP/1.1 405 Method Not Allowed");
    EXPECT_NE(response.find("allow: POST\r\n"), std::string::npos);
    // The length still describes the body a GET would have carried.
    EXPECT_NE(response.find("content-length: 23\r\n"), std::string::npos);
    EXPECT_TRUE(body_of(response).empty());
}

// A 404 is not a parse failure. The stream is still synchronised, so the client
// may ask for something else on the same connection -- which is why the routing
// statuses carry no connection: close.
TEST(ServeConnection, KeepsTheConnectionOpenAfterAFourOhFour) {
    auto pair = connected_pair();
    send_all(pair.first,
             "GET /missing HTTP/1.1\r\nHost: a.test\r\n\r\n"
             "GET /here HTTP/1.1\r\nHost: b.test\r\n\r\n");

    const std::string response = serve_and_read(pair, routing("/here"));

    EXPECT_EQ(response.find("HTTP/1.1 404 Not Found"), 0U);
    EXPECT_NE(response.find("HTTP/1.1 200 OK"), std::string::npos);
    EXPECT_EQ(response.find("connection: close"), std::string::npos);
}

// A handler's response reaches the client unaltered but for the length, which is
// what makes the seam worth having.
TEST(ServeConnection, SendsWhatTheHandlerReturned) {
    auto pair = connected_pair();
    send_all(pair.first, get_root);

    Router router;
    router.add(Method::Get, "/", [](const Request&) {
        auto response = text_response(201, "made\n");
        response.headers.add({"x-from-handler", "yes"});
        return response;
    });

    const std::string response = serve_and_read(pair, router);

    // 201 is not in the phrase table, and a handler is still entitled to send it:
    // the line keeps its space and drops the phrase.
    EXPECT_EQ(status_line(response), "HTTP/1.1 201 ");
    EXPECT_NE(response.find("x-from-handler: yes\r\n"), std::string::npos);
    EXPECT_EQ(body_of(response), "made\n");
}

// A client that half-closes without sending anything is finished, not broken, so
// it gets no reply at all -- not even a 400.
TEST(ServeConnection, WritesNothingWhenTheClientFinishesFirst) {
    auto pair = connected_pair();

    EXPECT_TRUE(serve_and_read(pair, Router{}).empty());
}

// A failed read is not a bad request: there is no head to reject, and nobody to
// tell either way. A receive deadline is the cheapest way to make reads fail on a
// socket that can still be written to, which is what separates this from a client
// that simply hung up.
TEST(ServeConnection, WritesNothingWhenTheReadFails) {
    auto pair = connected_pair();

    const timeval deadline{0, suseconds_t{50} * 1000};
    ASSERT_EQ(::setsockopt(pair.second.get(), SOL_SOCKET, SO_RCVTIMEO, &deadline, sizeof(deadline)),
              0);

    {
        Connection conn{std::move(pair.second)};
        serve_connection(conn, Router{});
    }

    std::array<char, 64> buf{};
    EXPECT_EQ(::recv(pair.first.get(), buf.data(), buf.size(), 0), ssize_t{0});
}

// The write fails with EPIPE and the loop has to stop on it. Reaching the end of
// this test at all is the assertion: a loop that ignored the failure would spin.
TEST(ServeConnection, StopsWhenTheClientIsAlreadyGone) {
    auto pair = connected_pair();
    send_all(pair.first, get_root);
    pair.first = Socket{-1};

    const Router router = routing("/");
    Connection conn{std::move(pair.second)};
    serve_connection(conn, router);

    SUCCEED();
}

}  // namespace
