#include "server/pipeline.hpp"

#include <carafe/http/handler.hpp>
#include <carafe/http/middleware.hpp>
#include <carafe/http/request.hpp>
#include <carafe/http/response.hpp>

#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

namespace {

using carafe::http::Handler;
using carafe::http::Method;
using carafe::http::Middleware;
using carafe::http::Next;
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

// Records when it runs relative to everything it wraps, so a test reads the order back as one string.
Middleware tracing(std::string& trace, std::string name) {
    return [&trace, name = std::move(name)](const Request& request, const Next& next) {
        trace += name + "-before ";
        Response response = next(request);
        trace += name + "-after ";
        return response;
    };
}

// Registration order is wrapping order: the first middleware registered is outermost, so its work before the call
// happens first and its work after the call happens last.
TEST(Pipeline, RunsMiddlewareAroundTheHandlerFirstRegisteredOutermost) {
    std::string trace;
    Pipeline pipeline;
    pipeline.add(Method::Get, "/", [&trace](const Request&) {
        trace += "handler ";
        return text_response(200, "");
    });
    pipeline.use(tracing(trace, "outer"));
    pipeline.use(tracing(trace, "inner"));
    Request request = request_for(Method::Get, "/");

    EXPECT_EQ(pipeline.respond(request).status, 200);
    EXPECT_EQ(trace, "outer-before inner-before handler inner-after outer-after ");
}

// Refusing is returning without calling next: the handler never runs, and what the middleware returned is the answer.
TEST(Pipeline, LetsAMiddlewareAnswerWithoutCallingNext) {
    int handled = 0;
    Pipeline pipeline;
    pipeline.add(Method::Get, "/", [&handled](const Request&) {
        ++handled;
        return text_response(200, "");
    });
    pipeline.use([](const Request&, const Next&) { return carafe::http::status_response(401); });
    Request request = request_for(Method::Get, "/");

    EXPECT_EQ(pipeline.respond(request).status, 401);
    EXPECT_EQ(handled, 0);
}

// What next returns is the middleware's to change before handing it back.
TEST(Pipeline, LetsAMiddlewareEditTheResponse) {
    Pipeline pipeline;
    pipeline.add(Method::Get, "/", answering("body\n"));
    pipeline.use([](const Request& request, const Next& next) {
        Response response = next(request);
        response.headers.add({"x-edited", "yes"});
        return response;
    });
    Request request = request_for(Method::Get, "/");

    const Response response = pipeline.respond(request);

    EXPECT_EQ(response.body, "body\n");
    EXPECT_EQ(response.headers.get("x-edited"), "yes");
}

// Routing happens before the chain, so a middleware can read the captures before deciding whether to call next.
TEST(Pipeline, ShowsMiddlewareTheRouteCapturesBeforeNext) {
    std::string seen;
    Pipeline pipeline;
    pipeline.add(Method::Get, "/users/<id>", answering("user\n"));
    pipeline.use([&seen](const Request& request, const Next& next) {
        seen = std::string{request.params.get("id").value_or("")};
        return next(request);
    });
    Request request = request_for(Method::Get, "/users/42");

    EXPECT_EQ(pipeline.respond(request).status, 200);
    EXPECT_EQ(seen, "42");
}

// The chain wraps dispatch, not only a matched handler, so a request no route claims still passes through it. An access
// log that missed 404s would miss exactly the requests worth reading.
TEST(Pipeline, PassesUnmatchedResponsesBackThroughMiddleware) {
    std::vector<int> statuses;
    Pipeline pipeline;
    pipeline.add(Method::Get, "/thing", answering("thing\n"));
    pipeline.use([&statuses](const Request& request, const Next& next) {
        Response response = next(request);
        statuses.push_back(response.status);
        return response;
    });
    Request missing = request_for(Method::Get, "/elsewhere");
    Request wrong_method = request_for(Method::Delete, "/thing");

    EXPECT_EQ(pipeline.respond(missing).status, 404);
    const Response refused = pipeline.respond(wrong_method);

    EXPECT_EQ(statuses, (std::vector<int>{404, 405}));
    EXPECT_EQ(refused.headers.get("allow"), "GET, HEAD");
}

// Each call to next runs everything after it again, handler included. That is what a retry needs, and it is also why
// a middleware that calls next twice by accident runs the handler twice.
TEST(Pipeline, RunsTheRestAgainEachTimeNextIsCalled) {
    int handled = 0;
    Pipeline pipeline;
    pipeline.add(Method::Get, "/", [&handled](const Request&) {
        ++handled;
        return text_response(200, "");
    });
    pipeline.use([](const Request& request, const Next& next) {
        static_cast<void>(next(request));
        return next(request);
    });
    Request request = request_for(Method::Get, "/");

    EXPECT_EQ(pipeline.respond(request).status, 200);
    EXPECT_EQ(handled, 2);
}

// A middleware passes something to the handler by handing next a modified copy. The route was chosen before the chain
// began, so the copy reaches that route even with a different target written into it.
TEST(Pipeline, SendsAModifiedCopyToTheRouteAlreadyChosen) {
    std::string user;
    Pipeline pipeline;
    pipeline.add(Method::Get, "/", [&user](const Request& request) {
        user = std::string{request.headers.get("x-user").value_or("")};
        return text_response(200, "");
    });
    pipeline.use([](const Request& request, const Next& next) {
        Request copy = request;
        copy.headers.add({"x-user", "alice"});
        copy.target = "/somewhere-else";
        return next(copy);
    });
    Request request = request_for(Method::Get, "/");

    EXPECT_EQ(pipeline.respond(request).status, 200);
    EXPECT_EQ(user, "alice");
}

// The Allow list has to describe the path that was routed. Building it from a forwarded copy's target would describe
// a path nobody asked the router about.
TEST(Pipeline, BuildsTheAllowListFromTheOriginalTarget) {
    Pipeline pipeline;
    pipeline.add(Method::Get, "/thing", answering("thing\n"));
    pipeline.use([](const Request& request, const Next& next) {
        Request copy = request;
        copy.target = "/unregistered";
        return next(copy);
    });
    Request request = request_for(Method::Delete, "/thing");

    const Response response = pipeline.respond(request);

    EXPECT_EQ(response.status, 405);
    EXPECT_EQ(response.headers.get("allow"), "GET, HEAD");
}

// A middleware is the caller's code as much as a handler is, so a throw from it is this request's 500 too.
TEST(Pipeline, AnswersAThrowingMiddlewareWithFiveHundred) {
    Pipeline pipeline;
    pipeline.add(Method::Get, "/", answering("unreached\n"));
    pipeline.use([](const Request&, const Next&) -> Response { throw std::runtime_error("middleware failed"); });
    Request request = request_for(Method::Get, "/");

    const Response response = pipeline.respond(request);

    EXPECT_EQ(response.status, 500);
    EXPECT_EQ(response.body, "500 Internal Server Error\n");
}

// The handler keeps its own catch inside the chain, so a middleware wrapping it sees a failing handler as an ordinary
// 500 response rather than an exception unwinding past it.
TEST(Pipeline, LetsAMiddlewareSeeAThrowingHandlersFiveHundred) {
    int seen = 0;
    Pipeline pipeline;
    pipeline.add(Method::Get, "/", [](const Request&) -> Response { throw std::runtime_error("handler failed"); });
    pipeline.use([&seen](const Request& request, const Next& next) {
        Response response = next(request);
        seen = response.status;
        return response;
    });
    Request request = request_for(Method::Get, "/");

    EXPECT_EQ(pipeline.respond(request).status, 500);
    EXPECT_EQ(seen, 500);
}

}  // namespace
