# Connection and App

The seams: joining a socket to the parser, the public entry point, and the
response a handler returns. Part of the [design notes](README.md).

## The join belongs to neither side

`Connection` reads bytes off a `Socket` and feeds them to a `RequestReader`, and
it lives in a third subsystem because it cannot live in either of the two it
joins. Putting a `Socket` into `http/` would end the parser's independence from
transport, which is the property that lets every parser test run on a string
literal with no descriptor in sight. Putting a `RequestReader` into `net/` would
invert the same dependency. So `src/server/` exists, and routing, handlers and
middleware will land beside it rather than in `http/`.

The socket, the parser and the read buffer are members rather than parameters
because all three are per-connection state with exactly the same lifetime. The
reader in particular holds the bytes of a *half-received* head between reads —
that is the whole reason it is a separate layer — so sharing one across two
connections would splice the tail of one client's head onto the front of
another's, assembling a request that nobody sent. Value membership makes that
unrepresentable rather than merely discouraged.

`ConnectionResult` carries two independent failure channels, and they are not
collapsible. A malformed head is answerable: the socket is fine, and the caller
owes the client a 400, 414 or 431 depending on which `RequestError` came back. A
failed read is not: there is nobody left to answer. One field could not say which
of those happened without inventing an error value that means "ask elsewhere".

The loop parses before it reads, and the order is load-bearing rather than
stylistic. A pipelining client sends two requests in one segment and then waits
for the first response. Reading first, the server would already hold the second
request in the reader's buffer and still block waiting for bytes that have
already arrived — a deadlock in which both sides are correct and neither moves.
Parsing first drains what is in hand before ever asking the kernel for more.

One trade is recorded rather than solved. A stream that ends part-way through a
head is reported as a finished connection, not a bad request. `RequestReader`
does not expose whether it is mid-head, and a peer that has hung up cannot be
told anything anyway. The case that would justify the distinction is a client
that half-closes and still reads, which nothing here can produce yet.

## App is the seam, and it is deliberately thin

Writing the first example forced the first public API decision, because nothing
public could run a server: `Listener`, `Socket` and `Connection` all live in
`src/`, and the README's boundary rule says only tests cross it. An example that
included internal headers would be demonstrating unsupported API to exactly the
people most likely to copy it. So `carafe::App` exists — the Flask-shaped entry
point the roadmap was always heading for, in its smallest honest form.

Everything with a decision in it lives below the seam. `serve_connection` does
the read-answer-repeat work and is tested over a `socketpair`; `App::run` is the
listen-and-accept loop and is not tested at all. That split is the point: the
untestable part is ten lines of glue with one branch, and it exists so that the
part worth testing has no listener in it.

`ECONNABORTED` continues the accept loop and every other error ends it. A
connection dying in the queue before it is taken is routine, while `EBADF` or
`EINVAL` means the listener is finished and retrying would spin hot on the same
error. This is the caller the accept note predicted would eventually justify an
`AcceptError` enum — and one caller with one special case still does not, so a
bare `errno` comparison is the right size for now.

Two limits are known rather than handled. `EMFILE` ends the server, though it is
transient: continuing would busy-loop, and doing better needs a backoff this has
no reason to grow yet. And connections are served serially with keep-alive on, so
a client that holds a connection open locks everyone else out. Both are the
concurrency milestone's motivation rather than oversights, and the example says
so in its own comment.

A smaller consequence, recorded because it cost a debugging round: switching
`AcceptResult::operator bool` to test `os_error` made `clang-tidy` lose its proof
that `*accepted.client` is safe, since the old spelling *was* the proof. The
habit that follows is worth keeping — at a dereference site, test the optional
rather than the result. `operator bool` answers "did it work", which is a
different question from "is there a payload here".

## Content-Length is not the caller's to get wrong

`Response` is a struct with three public fields, and that is the whole type. A
class earns its keep when some relationship between members must hold — `Headers`
lowercases on `add` so lookup can compare directly, `Socket` deletes its copy so
exactly one owner closes the fd, `Connection` keeps its socket and reader
together so two clients' bytes cannot be spliced. Ask what an inconsistent
`Response` would look like and there is no answer: any status, any headers, any
body is valid. Private members and setters that only assign would be ceremony
around a value with no rule. It also stays an aggregate, which matters because a
handler is what constructs one.

The one rule that does exist is about bytes, so it lives in `serialize`.
`Content-Length` is computed from `body.size()` and any the caller supplied is
skipped, because it is the single number a client cannot recover from when it is
wrong: too small and the response is truncated mid-body, too large and the
connection hangs waiting for bytes nobody will send. Two of them is worse still —
the client believes whichever it reads first and the ends silently disagree about
where the body stops. Overriding an explicit caller header is a real cost,
accepted because the failure it prevents is invisible and remote.

`with_body` is a parameter rather than a field for a reason the error path makes
concrete. `serve_connection` knows the method only when a request parsed; a
malformed head has none, which is why it is being rejected. As a field,
`error_response` would have to invent an answer to a question it cannot answer.
As a parameter it is simply not asked, and the default sends everything.

The HEAD rule stops being maintained by hand in the process. `response_for` used
to build the body even for HEAD purely so the declared length matched what a GET
would have sent — an invariant a later edit could quietly break by skipping the
work. Now the body is always built and the length always comes from it, and the
flag only decides how much goes on the wire. There is no path that computes one
without the other, so the comment warning about it is gone with the bug it
described.

Status is an `int` with a separate `status_message`, not an enum. The exhaustive
switch an enum buys is something `status_for` wants, not something a handler
wants, and a framework user is entitled to answer 429 without carafe having
enumerated it. Unknown codes get an empty phrase rather than a guess, which RFC
7230 permits — the space after the code belongs to the status line, not to the
phrase, so `HTTP/1.1 418 \r\n` is well formed and only stays that way if nothing
concatenates the space onto the phrase.

Field names go on the wire lowercased, because `Headers::add` lowercases what it
is given and a response whose own headers were capitalised would imply a
distinction HTTP does not make. HTTP/1.1 compares field names case-insensitively
and HTTP/2 requires them lowercase outright, so this is the spelling that stays
correct. Changing it caught something worth recording: the serve tests that
asserted `Connection: close` was *present* went red, and the one asserting it was
*absent* stayed green while testing nothing at all. A changed spelling announces
itself in presence assertions and hides in absence assertions.

## The seam has to hide its own router

`App` owns a `Router`, and `app.hpp` is not allowed to say so. The header is
public API — the README's rule is that anything under `include/carafe/` is
something a user may rely on — while `router.hpp` lives in `src/` and changed
shape as soon as path parameters landed: `Route` stopped storing a path and
started storing a compiled pattern, and `Match` grew a field. Including it would
have shipped `Router`, `Match` and `find` as supported surface by accident.

So `App` holds a `std::unique_ptr<server::Router>` behind a forward declaration.
That is pimpl with the router as its own impl, no extra struct, and it costs
exactly two out-of-line definitions. `~App()` must be declared in the header and
defaulted in `app.cpp`, because defaulting it inline would make the compiler emit
a `delete` of an incomplete type — `std::default_delete` has a `static_assert`
for precisely that. `App()` moves out of line for the opposite reason: `= default`
would compile happily and leave `router_` null, since `unique_ptr`'s default
constructor needs no complete type at all. One fails loudly, the other silently.
Copy and move are deleted rather than defined: nothing needs either, and a
moved-from `App` would still look runnable.

What the pimpl hides is `Router` specifically, not the vocabulary. `Handler`,
`Request` and `Response` stay fully visible, because those are what a handler is
written against. `Handler` earned its own public header on the way, being named
by both `app.hpp` and `router.hpp` with neither able to include the other — the
same two-consumer test `Headers` passed.

Routing also split what "an error" means. `error_response` attached
`connection: close` to everything, which was right while a malformed head was the
only failure the server could produce. A 404 is not that: the stream is still
synchronised and the client may ask for something else on the same connection.
So it became `parse_error_response`, and the rename is the point — leaving the
old name would have let the next person reach for it on the routing path and
quietly kill keep-alive for every missing page.

`text_response` moved from `serve.cpp` into the public header at the same time.
The response note said it would graduate once routing gave it a second caller;
routing did, and without it every handler in the example would assemble the same
four lines by hand.

One conformance gap was left open here rather than bolted onto the wiring. RFC
9110 says a 405 MUST carry an `Allow` header naming the methods the path does
support, and this commit sent none, because supplying it meant `Router`
reporting a set and that is a change to the routing interface. It landed in
the next commit; see [Refusing a method means naming the ones that
work](05-routing.md#refusing-a-method-means-naming-the-ones-that-work).

## The connection loop stopped knowing that routes exist

`serve_connection` was doing two jobs. Transport: reading until a request or a
failure, answering parse failures and a `408`, deciding whether to close, and
dropping the body of a `HEAD` response. Application: routing, binding
captures, calling the handler, and building a `404`, `405` or `500`.
Middleware has to wrap the second job and leave the first alone, and there was
no seam to wrap it at, so the two were separated before any middleware
existed.

`Pipeline::respond` is the application half, and the connection loop keeps the
rest. The split sits exactly where the loop stops needing to know that routes
exist: it hands over a parsed request and gets a response back. `HEAD` shows
where the line runs. The pipeline returns the `GET` handler's whole response,
body included, and the loop drops the body when it writes, because only the
loop knows it is writing a `HEAD` response, and the length it reports has to
describe the `GET` body.

There is one way in. `Pipeline::add` forwards to a router the pipeline keeps
private, and no constructor takes a router, so the router stays an
implementation detail of the pipeline in the same way `App` keeps it out of
its public header. Middleware registration will sit beside `add`, not beside a
router nothing outside can see.

`App` holds the pipeline through a `shared_ptr` and passes it to the pool as a
pointer to const, so every worker calls `respond` on one shared instance at
once. That is safe for the reason sharing the router was: nothing changes the
pipeline after `run` starts, and the only thing `respond` writes, the
captures, goes into a request each connection owns. `respond` takes the
request by non-const reference for exactly that write, and assigns the
captures unconditionally, so a miss leaves none behind in a request that
arrived carrying some.

`status_response` moved into the public header. The loop needs it for parse
failures and the `408`, and the pipeline needs it for the `404`, `405` and
`500`, which is the second-caller test `text_response` passed on its way to
the same header. Its body, the status and its phrase followed by a newline, is
now a documented contract with a test of its own, and middleware answering a
`401` or `403` will reach for it.

The pipeline is tested without a socket, which was half the point. The other
half of the evidence is the existing suite: every connection, pool and serve-
forever test was moved from a router to a pipeline, with nothing changed but
names and two comments, and passed. That is what shows no behaviour moved with
the code.
