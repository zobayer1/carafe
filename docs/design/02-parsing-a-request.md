# Parsing a request

How bytes become a request line and header fields, and what a failure reports.
Part of the [design notes](README.md).

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
