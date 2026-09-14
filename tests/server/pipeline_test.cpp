#include "server/pipeline.hpp"

#include <carafe/http/handler.hpp>
#include <carafe/http/request.hpp>
#include <carafe/http/response.hpp>

#include <stdexcept>
#include <string>
#include <utility>

#include <gtest/gtest.h>

namespace {

using carafe::http::Handler;
using carafe::http::Method;
using carafe::http::Request;
using carafe::http::Response;
using carafe::http::text_response;
using carafe::server::Pipeline;

// A request as the reader hands one over: method and target set, nothing bound. No socket is involved anywhere in this
// file, which is the point of the pipeline being its own type.
Request request_for(Method method, std::string target) {
    Request request;
    request.method = method;
    request.target = std::move(target);
    return request;
}

Handler answering(std::string body) {
    return [body = std::move(body)](const Request&) { return text_response(200, body); };
}

// What the handler returned comes back untouched: status, headers and body alike.
TEST(Pipeline, RespondsWithWhatTheHandlerReturned) {
    Pipeline pipeline;
    pipeline.add(Method::Get, "/", [](const Request&) {
        Response response = text_response(201, "made\n");
        response.headers.add({"x-test", "yes"});
        return response;
    });
    Request request = request_for(Method::Get, "/");

    const Response response = pipeline.respond(request);

    EXPECT_EQ(response.status, 201);
    EXPECT_EQ(response.body, "made\n");
    EXPECT_EQ(response.headers.get("x-test"), "yes");
}

// Captures are bound into the request itself before the handler runs, which is why respond takes it by reference: the
// handler reads them, and so does anything holding the request afterwards.
TEST(Pipeline, BindsTheRouteCapturesIntoTheRequest) {
    Pipeline pipeline;
    std::string seen;
    pipeline.add(Method::Get, "/users/<id>", [&seen](const Request& request) {
        seen = std::string{request.params.get("id").value_or("")};
        return text_response(200, "");
    });
    Request request = request_for(Method::Get, "/users/42");

    EXPECT_EQ(pipeline.respond(request).status, 200);
    EXPECT_EQ(seen, "42");
    EXPECT_EQ(request.params.get("id"), "42");
}

// The assignment is unconditional, so a miss leaves no captures behind even in a request that arrived carrying some.
// The reader clears its request between requests too; this pins the pipeline's half, which has to hold on its own.
TEST(Pipeline, ClearsCapturesARequestArrivedWithOnAMiss) {
    Pipeline pipeline;
    pipeline.add(Method::Get, "/users/<id>", answering("user\n"));
    Request request = request_for(Method::Get, "/missing");
    request.params.entries.push_back({"id", "stale"});

    EXPECT_EQ(pipeline.respond(request).status, 404);
    EXPECT_TRUE(request.params.entries.empty());
}

// No route claims the path at all: a 404 whose body names the status, and no Allow, since there is nothing to allow.
TEST(Pipeline, AnswersAnUnregisteredPathWithFourOhFour) {
    Pipeline pipeline;
    pipeline.add(Method::Get, "/here", answering("here\n"));
    Request request = request_for(Method::Get, "/elsewhere");

    const Response response = pipeline.respond(request);

    EXPECT_EQ(response.status, 404);
    EXPECT_EQ(response.body, "404 Not Found\n");
    EXPECT_FALSE(response.headers.contains("allow"));
}

// The path exists under other methods, so the answer is a 405 that names them: RFC 9110 makes Allow on it a MUST.
TEST(Pipeline, AnswersAKnownPathUnderAnotherMethodWithFourOhFiveAndAllow) {
    Pipeline pipeline;
    pipeline.add(Method::Get, "/thing", answering("get\n"));
    pipeline.add(Method::Post, "/thing", answering("post\n"));
    Request request = request_for(Method::Delete, "/thing");

    const Response response = pipeline.respond(request);

    EXPECT_EQ(response.status, 405);
    EXPECT_EQ(response.headers.get("allow"), "GET, HEAD, POST");
}

// HEAD reaches the GET handler and gets its whole response back, body included. Dropping the body belongs to the
// connection loop, which is how a HEAD's length still describes the GET body; a pipeline that dropped it here would
// break that.
TEST(Pipeline, AnswersHeadWithTheGetHandlersWholeResponse) {
    Pipeline pipeline;
    pipeline.add(Method::Get, "/", answering("the body\n"));
    Request request = request_for(Method::Head, "/");

    const Response response = pipeline.respond(request);

    EXPECT_EQ(response.status, 200);
    EXPECT_EQ(response.body, "the body\n");
}

// A handler is the caller's code, so a throw is this request's failure: a 500 naming the status and nothing more.
TEST(Pipeline, AnswersAThrowingHandlerWithFiveHundred) {
    Pipeline pipeline;
    pipeline.add(Method::Get, "/", [](const Request&) -> Response { throw std::runtime_error("internal detail"); });
    Request request = request_for(Method::Get, "/");

    const Response response = pipeline.respond(request);

    EXPECT_EQ(response.status, 500);
    EXPECT_EQ(response.body, "500 Internal Server Error\n");
}

}  // namespace
