# Design notes

Decisions taken while building carafe, with the reasoning behind them and the
alternatives that were rejected. Each entry is written to answer "why is it like
this" for someone reading the code later — including the author.

Add an entry when a choice was not obvious, or when the obvious choice was
rejected. Record what was given up, not only what was picked.

## C++17 is a choice, not a constraint

The toolchain in use is far newer than the floor in the requirements table, so
C++20 is available and unused. It would bring `std::span` for buffer handling
and `starts_with`/`ends_with` for parsing; C++23 would bring `std::expected`.
Staying on 17 is deliberate — writing that plumbing by hand is most of the point
of the exercise. Revisit only if a real problem needs a feature 17 lacks, not
for convenience.

## A header's location is the statement of support

The build lives at the top level — `CMakeLists.txt`, `CMakePresets.json`, the
`Makefile`, and the modules under `cmake/`. Source is split across `include/`,
`src/`, `tests/`, and `examples/`, prose lives in `docs/`, and each preset
builds into its own git-ignored `build/<preset>/`.

The boundary that matters is public versus internal. `include/carafe/` is the
library's API surface: anything there is something a user of the framework may
rely on, and it is included as `#include <carafe/foo.hpp>`. Everything else
stays in `src/`, beside the code it serves, and is included with quotes —
`#include "http/request_parser.hpp"`. That spelling works because `src/` is on
the include path `PRIVATE`ly, so it resolves inside the library and nowhere
else. Tests cross the boundary on purpose by putting `src/` on their own include
path; nothing else does, and a header's location is the statement of whether it
is supported.

The alternative is a header that says so in a comment, or a `detail` namespace
inside a public header. Both leave the file installable and includable, so the
statement is advice. Keeping internal headers out of `include/` makes it
structural: a consumer cannot reach `router.hpp` to depend on it by mistake,
which is the same reason `App` holds its `Router` behind a forward declaration.

Both trees group by subsystem — `http/`, `net/` and `server/` today — and
`tests/` mirrors that shape with one `*_test.cpp` per source file. Generated
headers, currently only `version.hpp` expanded from its `.in` template, land in
`build/<preset>/generated/carafe/` and are included exactly like hand-written
public ones; whoever includes them cannot tell the difference.

## Warnings are private, sanitizer flags are public

Two flag conventions are worth knowing before adding a target. Warnings are
`PRIVATE`, so they never leak into a consumer's build and every target must ask
for them by calling `carafe_target_warnings()`. Sanitizer and coverage flags are
`PUBLIC` on the `carafe` target, because ASan has to instrument every
translation unit that lands in one binary — so anything linking `carafe::carafe`
inherits them automatically and must *not* apply them again.

The asymmetry is the point. A warning is an opinion about carafe's own source
and a consumer is entitled to a different one; an instrumented build is a
property of the whole binary, and a half-instrumented one reports faults that
are artefacts of the mixture rather than bugs in either half.

## A parsed request owns its strings

`Request` holds owning `std::string` members. `std::string_view` is used heavily
*inside* the parser for zero-copy slicing and comparison, but owned strings are
materialized at the boundary when a field is committed.

This is not the zero-copy design, and it is chosen with that in mind. Views into
a growing connection buffer cannot be stored incrementally: the next `append`
may reallocate and dangle every view already kept. The two ways out are to
re-parse after each growth — which makes scanning quadratic in the number of
chunks, and the attacker picks that number — or to store realloc-stable
`{offset, length}` pairs and resolve them to views at the end. The second is
correct, and it makes `Request` a bag of integers that is meaningless without
the buffer it indexes into, with every accessor needing that buffer passed back
in. That API burden infects handlers, the router, and anything that wants to
keep a header past the life of the connection.

Owning is not a complexity regression — it is O(n), one extra pass over bytes
already scanned. The cost is allocation count, and small-string optimization
absorbs most of it, since header names and methods are nearly all under the
15-character inline threshold. Revisit once real socket buffers exist *and* a
benchmark says the copying matters; either alone is not enough.

Keeping a scan position across reads is a separate matter and not deferred with
this. Resuming the `\r\n\r\n` search from `scanned - 3` rather than from zero is
a hostile-input requirement, independent of who owns the bytes.

## Parse failures carry a reason

Parsing returns a small struct — an error enum plus the parsed value, with an
`explicit operator bool` — rather than throwing or returning a bare
`std::optional`. HTTP needs to know *which* failure occurred: a malformed
request line is 400, an unknown method is 501, an unsupported version is 505,
and `optional` discards exactly that. Exceptions were the other candidate and
were rejected because this is a hot path fed by hostile input, where malformed
requests are ordinary traffic rather than an exceptional condition.

The error enum names semantic failures, not status codes; mapping them to
responses belongs to the HTTP layer, so the parser stays usable by something
that serves no responses at all. The result struct is a hand-rolled
`std::expected` — it should be generalized into a `Result<T>` template once
there is a second use site to shape it, and not before.

The rule is conditional on there being more than one failure to tell apart.
`parse_header_field` has exactly one — a malformed field is 400 and nothing
else — so it returns a bare `std::optional` and inventing an error enum for it
would add a state no code path can produce. `LineReader` went the other way for
the opposite reason: it needed a *third* state, since "no line yet" is not a
failure. Each layer's return type is counted from its own states rather than
copied from its neighbour.

## The line splitter owns the terminator

`parse_request_line` receives the bytes of a line with CRLF already removed, and
never sees or strips a terminator itself. The alternative — tolerating a trailing
`\r` in the parser — means two components both know about terminators, and a bare
`\n` line ending behaves differently from `\r\n` for reasons no single file explains.

Stripping the terminator does not make the line free of CR and LF: a `\r` not
followed by `\n` survives a two-byte split and lands in the target, which is a
smuggling vector when a proxy and this server disagree about where the line ends.
The parser therefore rejects every control byte in the line, which also costs it
nothing to reject NUL and tab. Length capping stays with the splitter, since by
the time the parser is called the bytes are already buffered and the memory is
already spent.

## Field names are normalized, field values are not

Header field names are case-insensitive, so `parse_header_field` lowercases them
once at parse time rather than leaving every lookup to compare case-insensitively.
It also makes the wire format and the in-memory format agree with HTTP/2, which
mandates lowercase names outright. Values are left exactly as received: casing
carries meaning in base64 credentials, entity tags, and URLs, and a parser that
folds it is corrupting data rather than normalizing it.

Both the name and the value are validated against allowlists taken from the
grammar — `tchar` for names, `field-vchar` plus SP and HTAB for values — rather
than against a list of forbidden bytes. A denylist over an open set is a hole by
construction: an earlier version named CR, LF, and DEL, and let NUL through into
a value, which is a truncation vector the moment that string meets a C API. An
allowlist fails closed on the byte nobody thought of.

## `Headers` is a container, not a rulebook

`Headers` stores fields in arrival order and answers questions about them —
`get`, `contains`, `count`. It does not enforce HTTP's rules about *particular*
fields: it will happily hold two `Host` values, because rejecting them requires
knowing that `Host` is special, and a type that knows that is a type a response
cannot reuse.

So the policy lives one layer up, in `RequestReader`, which knows it is reading a
request and can therefore apply request rules — exactly one `Host` on HTTP/1.1,
none required on 1.0. `count()` exists for that caller rather than for
convenience. It is the same split that keeps `LineReader` from knowing whether a
line is a request line or a field, and `parse_header_field` from knowing which
field it just parsed.

A vector rather than a map, for three reasons that all point the same way: field
order is observable when reserialising, repeated fields are legal and a map
forbids them, and a linear scan wins at the sizes a bounded request head can
reach.

## One LineError, two statuses

`LineReader` reports `LineTooLong` without knowing what the line was for. That
single failure is a 414 if the line was the request line and a 431 if it was a
field — and only `RequestReader`, which tracks which phase it is in, can tell
them apart. Resolving that ambiguity is the clearest single reason the assembler
exists as its own layer rather than as a loop inside the caller.

The same shape recurs downward. `parse_header_field` returns a bare
`std::optional` because a malformed field is 400 and nothing else, and
`RequestReader` widens it into the error enum that also carries the two
too-long cases and the two head limits. Each layer names the failures it can
actually distinguish, and the layer above supplies the context to split them
further.

The head limits are `RequestReader`'s alone, for the same reason: a per-line cap
bounds one field, but nothing below this layer knows how many fields have
accumulated, or that 50,000 individually legal ones are not a legal head.

## Headers are listed, not declared as a file set

`CARAFE_HEADERS` is a plain variable passed to `add_library()`, which affects
only how IDEs display the project — compilation finds headers through include
paths. CMake 3.23 added `FILE_SET HEADERS`, which declares them as a real target
property and lets `install(TARGETS)` handle header installation directly. It is
available here and deliberately unused: its payoff is in install and export
rules, and this project has none. The day `find_package(carafe)` needs to work
from another project, migrate to a file set and add the export rules as one
change — they solve the same problem, and splitting them leaves the build half
converted.

## One descriptor, one owner

`Socket` holds a file descriptor and closes it exactly once. It is move-only
rather than copyable, because a copy means two owners and two closes, and the
second close lands on whatever descriptor the kernel handed out in the meantime.
That surfaces as traffic on the wrong connection rather than as a crash. Moving
is the only way to pass one around, so "who closes this" has a single answer at
every instant, and still will once connections are handed to threads.

The moved-from state is specified rather than merely valid: it holds
`invalid_fd` and owns nothing. The standard library promises only "valid but
unspecified" for its own types, which is enough for a container and not enough
here — a moved-from `Socket` is still destroyed, and its destructor must not
close a descriptor that now belongs to someone else. The tests assert that state
directly for exactly this reason.

Closing is never retried on `EINTR`. POSIX leaves the descriptor's fate
unspecified, but Linux releases it whether or not `close()` reports an error, so
a retry closes whatever another thread has since opened into the slot — the
double close the move semantics exist to prevent, reintroduced by the error
handling.

`get()` is present because the tests need it: asserting that the destructor
closed a descriptor requires knowing which one. `release()`, an early `close()`,
and a default constructor are absent for the same rule read the other way —
nothing calls them yet. `Listener` has since landed without wanting any of the
three, which is the argument for having waited.

## Binding is a function, not a constructor

`Listener` is a socket already listening plus the port it is bound to;
`listen_on` is the sequence of syscalls that produces one. Fusing the two — a
constructor that binds, or a static factory on the class — was the first attempt
and tied a knot: the factory returns a result type, the result type holds a
`Listener`, and the class ends up naming a type that names it back. Separating
the resource from the act of acquiring it dissolves that, and the header reads
top to bottom with nothing forward declared.

Failure is reported the way parse failures are, with an error enum and an
`explicit operator bool`, because a port already in use is an ordinary outcome
rather than an exceptional one — the same argument that keeps exceptions out of
the parser, and this would have been the only `throw` in the codebase. The enum
names the syscall that failed and nothing more; `os_error` carries the `errno`,
since `BindFailed` alone cannot separate a port already taken from one this user
may not have, and only the caller can turn that into a message. Reading `errno`
into the result before the local `Socket` is destroyed is load-bearing rather
than incidental: that destructor calls `close()`, which may overwrite it.

`SO_REUSEADDR` is set and `SO_REUSEPORT` is deliberately not. The first permits
binding only over a socket in TIME_WAIT, which is what lets a restart succeed
instead of failing for a minute after the previous process died. The second
permits a second live process to take a share of the traffic, which is a silent
way to lose requests to a stale binary. `SOCK_CLOEXEC` goes in the `socket()`
type argument rather than an `fcntl` afterwards, so there is no window in which
a concurrent `fork` copies the descriptor and keeps the port bound after this
process exits.

The listener binds `INADDR_ANY` over IPv4, and both halves are deferrals rather
than conclusions. A host argument means address parsing; IPv6 means a different
address type and a decision about `IPV6_V6ONLY` dual-stack. Each earns its own
commit. Flask's development server defaults to loopback and makes you ask for
anything wider, which is the safer default and the thing to revisit when the host
argument arrives.

`SO_REUSEADDR` needs a socket in TIME_WAIT to bind over, which is a property of
accepted connections rather than of listeners, so the test could not be written
until `accept()` existed. It now is, and it asserts first that a plain bind fails
with `EADDRINUSE` — otherwise a port that happened to be free would let the test
pass while proving nothing. `SOCK_CLOEXEC` on the listening socket stays
untested: the flag is one `fcntl(F_GETFD)` away, but nothing exposes that
descriptor to ask, and adding an accessor with no other caller would buy a test
at the price of API. The accepted socket is checked that way already.

The three error returns are left uncovered rather than marked excluded from
coverage: they are untested, not unreachable, and an exclusion marker would turn
a true signal into a fake 100%.

## A signal is not an outcome

`accept()` is a member where `listen_on` is a free function, and the reversal is
not an inconsistency. What forced `listen_on` out of the class was that its
result type *contains* a `Listener`; `AcceptResult` contains a `Socket`, so no
type names one that names it back and the header still reads top to bottom.
Meanwhile `accept` needs the descriptor only `Listener` owns, and a free function
could reach it only by exposing the fd — which is the one thing `Socket` exists
to prevent.

There is no `AcceptError` to match `ListenError`. That enum earns its place by
saying which of four syscalls failed, a question `errno` cannot answer; here
there is one call, so `errno` is the whole story and an enum with a single
meaningful value would be ceremony. A real classification does exist —
`ECONNABORTED` means drop this connection and keep serving, `EMFILE` means back
off, `EBADF` means the listener itself is finished — but that is a policy, and
its only caller is an accept loop that has not been written. It gets named here
and deferred, the same way `release()` was left off `Socket`.

`accept4` rather than `accept`, because descriptor flags are not inherited across
an accept: the connected socket arrives without `SOCK_CLOEXEC` even though the
listening socket has it. Plain `accept` would quietly undo the care taken one
function earlier.

`EINTR` is retried inside the function, and the retry is not bounded. The
temptation to cap it comes from reading the loop as error handling, which it is
not — nothing failed. The thread was parked in the kernel waiting for a
connection, a signal arrived, and the kernel returned early so a handler could
run. Continuing re-enters the same wait. A trial limit exists to stop a loop
from spinning hot on a cheap failing call; every iteration here blocks, consumes
nothing, and turns over only as fast as signals arrive.

Nor would a limit buy anything. After N interruptions the function would hand
back `EINTR` to a caller who asked for a connection and has not got one, so the
caller loops — the same unbounded loop moved up a level, plus an error value
meaning "ask again", which is precisely what the loop was already doing. The
pathological case, a repeating `SIGALRM` whose handler was installed without
`SA_RESTART`, does starve `accept`, and a cap does not fix that either; the fix
is `SA_RESTART` or a blocked signal mask, and that is the program's business
rather than this function's.

What this gives up is worth stating plainly: swallowing every `EINTR` means a
signal can no longer break a thread out of a blocking `accept`, which is one
classical way to shut a server down. The trade is accepted because the better
lever is to `shutdown()` or close the listening descriptor, which makes `accept`
return `EINVAL` or `EBADF` — reported through `os_error` rather than swallowed.
The blocking behaviour itself is deliberate for now; non-blocking accept is a
question for the concurrency milestone, not this commit.

## End of stream is not a failure

`Socket::read` has three outcomes where most of the codebase has two: bytes
arrived, the peer closed, or the call failed. The middle one is the whole reason
the type is shaped the way it is. A clean close is how every well-behaved client
finishes a connection, so folding it in with errors would make the single most
ordinary event on a socket look like a fault. `explicit operator bool` therefore
tests `os_error == 0` and stays *true* at end of stream, and the caller asks the
second question separately by checking whether `bytes` is engaged. Two questions
that cannot be collapsed into one are given two places to ask.

That is the same shape as `LineResult` — an error enum, an optional payload, and
`nullopt` meaning "nothing this time" rather than "something went wrong". There
is no `ReadError` to go with it, for the reason `AcceptResult` has none: one
syscall, so `errno` is the whole story and an enum would have a single
meaningful value.

The payload is a `std::string_view` into the caller's buffer rather than a byte
count. Both carry the same information, but only one of them is directly usable:
`reader.append(*result.bytes)` against `reader.append({buffer, result.bytes})`,
where the second rebuilds the pair by hand at every call site and can get it
wrong at any of them. The cost is a lifetime rule — the view dies with the
buffer — which is the same rule `LineReader::next_line` already imposes, and it
is why the buffer belongs to the caller in the first place.

`read` is not `const`, and `readability-make-member-function-const` is
suppressed rather than obeyed. A `const Socket&` reads as something safe to hand
around, and these bytes are gone from the stream once taken: no second reader
gets them back. `std::istream::read` is non-const for the same reason. `EINTR`
is retried inside, following `accept` rather than re-deciding.

One trap is recorded rather than fixed. `recv` with a count of zero returns
zero, which this reports as end of stream — so a caller that ever passes a full
buffer would hang up on a live connection. No caller does, and the `string_view`
design leaves the honest fix available when one might: an engaged but empty view
means "read nothing, still open", which a bare count could not express.

## A short write is an obligation

`Socket::write` sends everything or reports why it could not, where `read`
returns whatever happened to be there. The asymmetry is deliberate. A short read
is *information* — bytes arrived, here they are, ask again when you want more. A
short write is an *obligation*: bytes did not go out, and someone must send the
rest. Obligations are what call sites forget, and a forgotten tail is a truncated
response that reads like a bug in the client. So the loop lives here once,
instead of at every call site that ever writes a byte.

It is named `write` rather than `write_all` because there is no other write to
tell it apart from. Naming a distinction before it exists is the same thing that
kept `release()` off `Socket`; if a partial-write primitive is ever needed, that
is the one that gets the qualified name.

`::send` with `MSG_NOSIGNAL`, never `::write`. Writing to a socket whose peer has
gone raises `SIGPIPE`, whose default disposition kills the process — which is how
a server dies when one client hangs up early. The flag turns that into `EPIPE` in
`errno`. The alternatives were rejected: `signal(SIGPIPE, SIG_IGN)` works but a
library has no business changing the host process's signal disposition, and
`SO_NOSIGPIPE` exists only on the BSDs. This is the reason `recv` was chosen over
`read` on the other side too — keeping the flags argument in view.

Progress is tracked by shrinking the view with `remove_prefix` rather than by an
offset variable. It compiles to two instructions whose flags already answer
`!bytes.empty()`, so the loop condition is free, and it leaves exactly one
statement that advances and one that decides completion. An explicit
"did we send it all" check would put the completion test in two places that have
to agree. The `!bytes.empty()` guard is load-bearing rather than cosmetic: it is
what keeps a zero-length argument away from `send`, where a zero return would
leave the loop with no progress to make.

The partial-write path also creates a trap worth naming, because the obvious
implementation falls into it. `errno` is not cleared by a successful call, so
code that treats a short return as failure reads whatever the last failure
anywhere in the process left behind — reporting an error that did not happen, or,
if the stale value happens to be `EINTR`, resending a prefix that already went
out. Only `-1` means failure, which is what the branch tests.

One discovery from testing: on a blocking socket a signal produces `EINTR` only
when it beats the first byte out. Arriving later, it comes back as a short count
instead. Both paths therefore exist and are covered separately — the retry needs
the send buffer stuffed full beforehand so the call blocks with nothing
transferred, while the resume path falls out of any write large enough to be
interrupted mid-flight.

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
support, and this commit sent none, because supplying it meant `Router` reporting
a set and that is a change to the routing interface. It landed in the next
commit; see *Refusing a method means naming the ones that work*.

## Not matching is two different answers

`Router::find` returns a handler pointer and a `path_matched` flag, because a
request that finds no handler has two distinct fates. A path nobody registered is
a 404. A path registered under another method is a 405, and the client learns
something useful from the difference — that the resource exists and the verb was
wrong. A null pointer alone cannot say which, so the flag carries the one bit the
pointer cannot. It is meaningless when the pointer is non-null, which is why
`operator bool` tests only the pointer: one field per independent channel, the
rule `ReadResult` and `ConnectionResult` already follow.

Routes live in a `vector` scanned in registration order rather than a hash map
keyed by path. Static paths would suit a map, but path parameters are the next
step and patterns have to be tried in order — starting with the structure that
generalises avoids replacing the storage almost immediately. Two rules fall out
of the scan for free. Registering a path twice keeps the first handler, because
the exact match returns at once, which makes a duplicate harmless rather than an
error `add` would need a channel to report. And an explicit HEAD route beats the
HEAD-to-GET fallback whatever order they were registered in, because the fallback
is remembered and returned only after the loop.

That fallback is RFC 9110's definition made structural: HEAD is GET without the
body, so registering a GET route answers both. It runs one way only. A GET is
never served by a handler written for HEAD, which is entitled to compute nothing
at all.

`find` cuts the target at the first `?` rather than expecting a stripped path,
and this is expedient rather than principled. Splitting a request-target is
parsing's job, and it belongs in `Request` alongside the query parameters that
nothing exposes yet. Until then the router does it, because the alternative fails
silently: a caller who forgets sees a 404 with nothing to suggest why, and
`find(method, target)` gives no hint that `target` is not what it says. Keeping
the cut inside also keeps the query cases testable without a socket.

The handler pointer aliases into the route vector, so `add` invalidates it. That
is the same contract `Headers::get` documents, and it holds for the same reason
in practice: routes are registered before the listener starts, never during.
Returning the `std::function` by value would close the hole at the cost of a copy
— possibly an allocation — on every request.

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

## Refusing a method means naming the ones that work

RFC 9110 makes `Allow` on a 405 a MUST, not a courtesy, and the reason is
practical: a bare 405 leaves the client to rediscover the interface one verb at a
time. So `Router` grew `allowed_methods(target)` and the responder turns what it
returns into a header.

The set is a second query rather than a field on `Match`. Putting it in `Match`
would have built a vector on every lookup to serve a code that almost never
fires, and the successful path — the one every request takes — would pay for it.
A separate call allocates only where the answer is read.

That choice has a cost, and it is the reason `matches` exists. Two functions now
scan the same table asking the same question, and today that question is
`pattern == path`, which is barely a function at all. It stops being barely a
function the moment path parameters land, and two copies of a pattern matcher
that must agree is exactly the bug this project would rather not write. Factoring
it was the mitigation that made the second-query trade defensible; skipping it
would have taken the cost without it. It takes two `string_view`s rather than a
`const Route&` for the ordinary reason that `Route` is private and nested, but
also the better one: what it decides is about paths, not routes.

`allowed_methods` lists HEAD wherever GET is registered, because `find` really
does answer HEAD from a GET route. The two have to agree — an `Allow` naming a
method the router would then refuse is worse than no header at all, since the
client has no way to tell it was lied to except by trying. HEAD derives from GET
and from nothing else: a bodiless POST is not a thing, and the fallback in `find`
tests for GET specifically. OPTIONS is the opposite trap. RFC 9110 lets a server
support it, plenty do, and carafe does not — so it must not appear, however
conventional it looks.

The header goes on the 405 and not on the 404. The field names the methods of a
resource, and a path with no routes has no resource to describe; an empty `allow:`
would assert that something is there and serves nothing.

`method_name` sits in `request.hpp` beside the `Method` enum, and it got there by
being written in the wrong place first. It began in `serve.cpp`'s anonymous
namespace, on the argument that the header assembly was its only caller and the
layering read well — the router says which methods, the responder says how they
are spelled. Coverage disagreed. A table of nine string literals is exactly where
a typo hides, and hidden in `serve.cpp` the only way to reach an entry was to
register that method and request a different one: at most eight names per test,
never the fallback, and five of them uncovered in practice. Testability is a
caller, and it wanted the function somewhere a test could name it.

`status_message` was the precedent that should have settled it earlier. Same
shape, same risk, and it is public with a nine-line test pinning every phrase —
while every one of its non-test callers is internal. So "only internal callers"
was never what kept a function out of `include/` here, and the rule the two now
share is the honest one: a vocabulary function over a public type belongs with
the type.

## A pattern is compiled once, and a request path never is

`Route` stores a `Pattern` — a vector of `Segment`, each either literal text or
the name a capture binds to — built by `add` at registration. Matching walks
pattern and path in lockstep, cutting the path on `/` as it goes, and never
re-parses the pattern it was handed.

The tempting symmetry is to compile the request path too and compare the two
vectors. It is wrong twice over. It allocates a string per segment per route per
request, on the hot path, to answer a question that needs no allocation at all
when a route fails on its first segment. And it reads `<id>` in an incoming path
as syntax, when a request path is data: a client asking literally for
`/users/<id>` must not be treated as having written a pattern. Matching is
asymmetric even though equality is not.

`Segment` owns a `std::string` rather than viewing into the registered path, and
that is not a style preference. Routes live in a `vector` that reallocates, and
even without reallocation a moved `Route` moves its `std::string` — which for a
short string means SSO copies the bytes into the new object's inline buffer and
leaves every view pointing at the dead original. A view into stored text is safe
only when the text has a stable address, and nothing here gives it one.

`Segment` and `Pattern` sit at namespace scope in `router.hpp` rather than nested
inside `Router`. `matches` and `compile` are free functions in `router.cpp`'s
anonymous namespace and have to name `Segment` in their signatures, which a
private nested type forbids. `Route` stays private and nested, because `add` is
the only way to build one.

One `matches` serves both `find` and `allowed_methods`, which is what makes a 405
on a parameterised path come out right without a line of extra work. It also has
to. Two definitions of "this route serves this path" would eventually disagree,
and the shape of that failure is an `Allow` header naming a method the router
then refuses — worse than no header, because the client acts on it.

## A parameter stands for exactly one segment, and never for nothing

`/users/<id>` matches `/users/42` and not `/users/42/posts`, because the walk
requires pattern and path to run out together. It also does not match `/users/`.
A parameter that bound the empty string would hand every handler an `id` naming
no user and make the same guard everyone's problem, so an empty piece is a
failure to match — and `/users/` is then a 404 rather than a 405, since no route
claimed the path at all. Flask decides both the same way.

The `<name>` syntax cannot collide with a path a conforming client could send:
RFC 3986 excludes `<` and `>` from a URI path outright, so a literal segment that
would be misread as a parameter cannot arrive over the wire. `compile` treats a
half-bracketed segment, and an empty `<>`, as literal text rather than rejecting
them. That keeps `add` free of an error channel it would otherwise need, and the
cost is bounded: a pattern that cannot match anything is a mistake its author
sees the first time they curl it.

Registration order decides between two patterns that both match, exactly as it
already decided between duplicate static paths — the scan returns on the first
hit. So a literal `/users/me` has to be registered before `/users/<id>`, or the
parameter swallows it. Flask's specificity ranking is the alternative, and it is
a body of rules to learn and explain in exchange for saving one line of ordering.

## No value is not an empty value

`Params::get` returns `std::optional<std::string_view>`, following `Headers::get`
rather than answering `""` for a name nothing bound. The empty string is not free
to use as a sentinel here in the way it looks: it means "bound to nothing", and a
type has to be able to promise that no real value collides with it.

`Router` can make that promise — a parameter binds at least one character, by the
rule above. `Params` cannot. It is an open aggregate whose `entries` anyone may
append to, so it has no way to enforce what its values look like, and a `get`
that returned `""` for both cases would be lying about a guarantee it does not
own. Closing the struct into a class to make the sentinel honest costs an
invariant, a constructor, and an error channel `add` would then need — all to
avoid an `optional` that already says the right thing.

The one deliberate divergence from `Headers::get` is case. Field names are
case-insensitive because a client picks the spelling and RFC 9110 says the server
must not care. A parameter name is written twice by the same person, in the
pattern and in the handler, so `<id>` and `get("ID")` is a typo — and matching it
would hide the typo rather than the difference.

## The captures ride on the request

`Request` grew a `Params params` member, filled by the router after the head is
parsed. Every other member came off the wire; this one did not, which is why it
carries a comment saying so. A reader of `request.hpp` has no other way to tell.

The alternative was a `RoutedRequest` wrapping a `Request` and its captures,
keeping the parsed request honest about holding only what the client sent. It
changes every handler signature to gain a distinction handlers do not care about,
and Flask puts the same thing on the request for the same reason.

`serve_connection` assigns the captures unconditionally rather than only on a
match, because an unmatched request captured nothing and moving an empty `Params`
costs less than the branch that would avoid it. That assignment is also what
keeps a pipelined connection clean: the second request cannot inherit the first
one's captures, because it is overwritten before any handler sees it. The reader
resets its `Request` after handing one over, which would guard the same thing,
but the two are redundant — removing the reset entirely fails no test. The
guarantee is worth pinning at the seam that actually holds it.
