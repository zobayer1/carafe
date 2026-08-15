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

Two behaviours here are recorded as untested rather than quietly assumed.
`SO_REUSEADDR` needs a socket in TIME_WAIT to bind over, which is a property of
accepted connections rather than of listeners, so the test only became writable
once `accept()` existed; it is scheduled for the commit that adds the `Listener`
accessors. `SOCK_CLOEXEC` on the listening socket is one `fcntl(F_GETFD)` away,
but nothing exposes that descriptor to ask — the accepted socket is checked that
way already. The three error returns are likewise left uncovered rather than
marked excluded from coverage: they are untested, not unreachable, and an
exclusion marker would turn a true signal into a fake 100%.

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
