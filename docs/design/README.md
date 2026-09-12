# Design notes

Decisions taken while building carafe, with the reasoning behind them and the
alternatives that were rejected. Each entry is written to answer "why is it like
this" for someone reading the code later — including the author.

Add an entry when a choice was not obvious, or when the obvious choice was
rejected. Record what was given up, not only what was picked.

The notes are split by subject into the numbered files below. A file's number
is the order its subject first appeared in the history, and entries inside a
file keep the order they were written, so reading everything in sequence
retraces how the project was built.

Add an entry at the end of the file its subject belongs to. A subject that
fits none of them starts a new file with the next number, listed here.

[../architecture.html](../architecture.html) draws the structure these notes
argue for.

## [01 Build and tooling](01-build-and-tooling.md)

The language standard, where headers live, how flags reach a target, and what
the gates check.

- [C++17 is a choice, not a constraint](01-build-and-tooling.md#c17-is-a-choice-not-a-constraint)
- [Headers are listed, not declared as a file set](01-build-and-tooling.md#headers-are-listed-not-declared-as-a-file-set)
- [A header's location is the statement of support](01-build-and-tooling.md#a-headers-location-is-the-statement-of-support)
- [Warnings are private, sanitizer flags are public](01-build-and-tooling.md#warnings-are-private-sanitizer-flags-are-public)
- [The gate could not see the headers it guarded](01-build-and-tooling.md#the-gate-could-not-see-the-headers-it-guarded)

## [02 Parsing a request](02-parsing-a-request.md)

How bytes become a request line and header fields, and what a failure reports.

- [A parsed request owns its strings](02-parsing-a-request.md#a-parsed-request-owns-its-strings)
- [Parse failures carry a reason](02-parsing-a-request.md#parse-failures-carry-a-reason)
- [The line splitter owns the terminator](02-parsing-a-request.md#the-line-splitter-owns-the-terminator)
- [Field names are normalized, field values are not](02-parsing-a-request.md#field-names-are-normalized-field-values-are-not)
- [`Headers` is a container, not a rulebook](02-parsing-a-request.md#headers-is-a-container-not-a-rulebook)
- [One LineError, two statuses](02-parsing-a-request.md#one-lineerror-two-statuses)

## [03 Sockets](03-sockets.md)

Owning a descriptor, listening on a port, and reading and writing bytes.

- [One descriptor, one owner](03-sockets.md#one-descriptor-one-owner)
- [Binding is a function, not a constructor](03-sockets.md#binding-is-a-function-not-a-constructor)
- [A signal is not an outcome](03-sockets.md#a-signal-is-not-an-outcome)
- [End of stream is not a failure](03-sockets.md#end-of-stream-is-not-a-failure)
- [A short write is an obligation](03-sockets.md#a-short-write-is-an-obligation)

## [04 Connection and App](04-connection-and-app.md)

The seams: joining a socket to the parser, the public entry point, and the
response a handler returns.

- [The join belongs to neither side](04-connection-and-app.md#the-join-belongs-to-neither-side)
- [App is the seam, and it is deliberately thin](04-connection-and-app.md#app-is-the-seam-and-it-is-deliberately-thin)
- [Content-Length is not the caller's to get wrong](04-connection-and-app.md#content-length-is-not-the-callers-to-get-wrong)
- [The seam has to hide its own router](04-connection-and-app.md#the-seam-has-to-hide-its-own-router)

## [05 Routing](05-routing.md)

Matching a request to a handler: methods, patterns, parameters and converters.

- [Not matching is two different answers](05-routing.md#not-matching-is-two-different-answers)
- [Refusing a method means naming the ones that work](05-routing.md#refusing-a-method-means-naming-the-ones-that-work)
- [A pattern is compiled once, and a request path never is](05-routing.md#a-pattern-is-compiled-once-and-a-request-path-never-is)
- [A parameter stands for exactly one segment, and never for nothing](05-routing.md#a-parameter-stands-for-exactly-one-segment-and-never-for-nothing)
- [No value is not an empty value](05-routing.md#no-value-is-not-an-empty-value)
- [The captures ride on the request](05-routing.md#the-captures-ride-on-the-request)
- [A type on a parameter is a question about shape](05-routing.md#a-type-on-a-parameter-is-a-question-about-shape)
- [A capture that names a subtree has to stay inside it](05-routing.md#a-capture-that-names-a-subtree-has-to-stay-inside-it)

## [06 Paths and escapes](06-paths-and-escapes.md)

What a path means before any route sees it: percent-escapes and normalisation.

- [Splitting comes first, decoding second](06-paths-and-escapes.md#splitting-comes-first-decoding-second)
- [A precondition nobody can check is a precondition worth handling](06-paths-and-escapes.md#a-precondition-nobody-can-check-is-a-precondition-worth-handling)
- [Three spellings of one path were three routes](06-paths-and-escapes.md#three-spellings-of-one-path-were-three-routes)

## [07 Bodies and persistence](07-bodies-and-persistence.md)

Framing a body, refusing one without losing the stream, and deciding when a
connection ends.

- [A body is framed by the head, and by nothing else](07-bodies-and-persistence.md#a-body-is-framed-by-the-head-and-by-nothing-else)
- [One declared length, or none at all](07-bodies-and-persistence.md#one-declared-length-or-none-at-all)
- [Refusing a request is not the same as losing the stream](07-bodies-and-persistence.md#refusing-a-request-is-not-the-same-as-losing-the-stream)
- [Politeness has a ceiling](07-bodies-and-persistence.md#politeness-has-a-ceiling)
- [The client decides when the connection ends](07-bodies-and-persistence.md#the-client-decides-when-the-connection-ends)
- [A coding list is a decision table, not a feature flag](07-bodies-and-persistence.md#a-coding-list-is-a-decision-table-not-a-feature-flag)
- [The body is the thing that is never declared](07-bodies-and-persistence.md#the-body-is-the-thing-that-is-never-declared)

## [08 Serving many clients](08-serving-many-clients.md)

Threads, the accept loop that must outlive its failures, and the bounded pool.

- [One slow client was every client](08-serving-many-clients.md#one-slow-client-was-every-client)
- [Only the listener failing may end the server](08-serving-many-clients.md#only-the-listener-failing-may-end-the-server)
- [A bounded pool is the outage again with a number on it](08-serving-many-clients.md#a-bounded-pool-is-the-outage-again-with-a-number-on-it)
- [A number the caller cannot reach is the library's number, not theirs](08-serving-many-clients.md#a-number-the-caller-cannot-reach-is-the-librarys-number-not-theirs)

## [09 Deadlines](09-deadlines.md)

Bounding how long a client may hold a worker while silent, while sending, and
while being sent to.

- [A connection that says nothing still costs something](09-deadlines.md#a-connection-that-says-nothing-still-costs-something)
- [A deadline that every byte renews is not a deadline](09-deadlines.md#a-deadline-that-every-byte-renews-is-not-a-deadline)
- [A short write is what the deadline was waiting for](09-deadlines.md#a-short-write-is-what-the-deadline-was-waiting-for)
