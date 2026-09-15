# Middleware

Acting on every request: the wrapping shape, where it runs, and what it can
and cannot see. Part of the [design notes](README.md).

## One shape for before, after and instead

Nothing let an application act on every request at once: log it, check a
credential, add a field to whatever goes back. Before and after hooks were the
obvious shape and were not taken. A middleware that receives `next` and
chooses when to call it covers both, and two things hooks cannot: it refuses
by returning without calling `next`, and it can put a `try` around the call.
One concept replaces three.

It runs after routing and wraps dispatch. So it sees the route's captures, and
it sees every `404` and `405` come back, which is exactly what an access log
needs. It never sees a parse failure or a `408`, because neither produces a
request object to hand over. Registration order is wrapping order, with the
first registered outermost.

`Next` is sixteen bytes: a pointer to the request's `Exchange` and an index
into the chain. Only the pipeline can build an `Exchange`, which lives on its
stack for the length of one `respond`, so only the pipeline can build a
`Next`, and a `Next` kept past the call it was handed to points at nothing. A
`std::function` built for each hop was the alternative, and it would have put
an allocation per middleware on every request.

Two catches, not one. A middleware is the caller's code as much as a handler
is, so a throw from anywhere in the chain becomes this request's `500`. The
handler keeps its own catch as well, and that is what lets an outer middleware
see a failing handler's `500` as an ordinary response: the access log records
a `500` rather than never learning the request happened.

`next` may be called more than once, and each call runs everything after it
again, handler included. That is what retrying or falling back needs, and
refusing it would take per-request bookkeeping that protects nobody. Two
middleware in sequence, a timer and an auth check say, do not need it: each
calls `next` once, and registration order does the chaining.

The route is chosen before the chain begins. A middleware can hand `next` a
modified copy of the request, which is how it passes something to the handler,
a header naming the authenticated user for instance, and that copy reaches the
route already chosen. The `Allow` list on a `405` is built from the original
target for the same reason, so it describes the path that was actually routed.

The trap is the target. A middleware sees `request.target` as the client sent
it, while the router matched the normalised path. A check for an `/admin`
prefix on the target is walked around by `/x/../admin/secret`, which the
router serves from `/admin/secret`. The example's auth middleware checks the
method instead. A middleware that genuinely needs to know the route needs per-
route middleware or the normalised path on the request, and both wait for
something that needs them.

Middleware runs on every worker at once, so anything it keeps is shared. The
example's access log builds each line whole and writes it in one insertion;
eight threads writing 160,000 such lines produced no malformed one. RFC 9110
§15.5.2 makes `www-authenticate` a MUST on a `401`, so the example's refusal
sends it.
