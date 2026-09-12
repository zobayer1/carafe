# Routing

Matching a request to a handler: methods, patterns, parameters and converters.
Part of the [design notes](README.md).

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

## A type on a parameter is a question about shape

A path parameter took any segment at all, so `/users/<id>` answered
`/users/42` and `/users/bob` alike and a handler wanting a number had to
decide what to do with one that was not. `<int:id>` moves that question to
where the routing decision already is: `/users/<int:id>` and `/users/<name>`
can both be registered, and the shape of the segment picks one.

The converter constrains matching and nothing else. A capture is still text,
and a handler that wants an integer parses it. Converting instead would mean a
variant in the public `Params`, a typed accessor beside `get`, and an answer
for what happens when a segment is too long for the type. None of that is what
`int:` is for, and the disambiguation works on shape alone.

So `int` means one or more ASCII digits. Not a sign, since a negative id in a
path is nearly always a bug, and no range check, since nothing is being
converted. Leading zeros match and survive into the capture, because `007` is
a key rather than a number to be tidied.

The test runs before the capture decode, and normalisation is why that is
enough. Digits are unreserved, so `/users/%34%32` is already `/users/42` by
the time the walk sees it, while `%2B42` keeps its escape and fails the test
for being no kind of digit.

Precedence stays where it was: the first pattern registered that matches wins,
and a converter earns no priority. Ranking by specificity would mean deciding
whether `<int:id>` is more specific than a literal, and then defending that
order against every converter added later. Registration order needs no such
defence, and it is already what a repeated path does.

Nothing new is refused, because `add` has no channel to refuse on. An unknown
converter or an empty name falls back to literal text, which is already what
`<>` and a half-bracketed segment do, so `/users/<integer:id>` is a path
nothing will ask for rather than an error nobody can see.

The enum is where the next converter will be caught. `Capture` has no `Rest`
yet, and when it gains one both switches over it, the walk and the predicate
that says whether a segment binds a name, stop compiling until it is
classified. That is the same guarantee the method and failure switches already
rely on, and it is why the predicate is a switch rather than a comparison
against `None`.

## A capture that names a subtree has to stay inside it

Every parameter so far stood for one segment, so a route could not say "this
directory and anything under it". `<path:rest>` can: it takes every segment
that is left, and a file handler is the obvious thing that will consume it.
That is what makes this converter different from the others. Its value is not
text a handler looks at, it is a location a handler joins under a directory,
so the question it has to answer is not what it matches but where it can lead.

The walk was built on lockstep, one piece against one pattern segment, and a
rest breaks that by consuming everything at once. So it is taken before the
per-segment check rather than inside it, and it ends the walk. A pattern with
a segment after the rest then needs no validation at all: the walk runs out of
path with pattern left over, and the length check that already existed reports
a miss. `add` still has no error channel and still needs none.

The first version replaced the switch with an if chain, since only two of its
arms did anything. That traded away the reason the switch existed. A switch
with no default refuses to compile until a new converter says what it accepts;
the chain compiles clean and lets it accept anything non-empty, which a probe
confirmed with a converter that took the text `not-hex-at-all`. The rest block
had also pushed `matches` past clang-tidy's cognitive complexity threshold,
with the switch or without it. Moving the per-segment rule into `accepts`, one
return per arm, fixed both: the loop got shorter and the switch got its
guarantee back. `accepts` answers false for a rest, so if the rest block is
ever moved below the call the route misses rather than accepting everything.

The decision that matters is refusing a separator that appears during
decoding. Normalisation deliberately leaves `%2F` escaped, so the path keeps
the boundaries the client actually sent. A capture is then decoded, and for
`/files/a%2F..%2Fb` that yields `a/../b`: the dot segment normalisation
removed from the path is back inside the value. That was already true of a
single-segment capture, and it did not matter while nothing treated one as a
location. It matters for a rest. The check counts separators before and after
decoding, and since decoding can only add them, any difference means a
boundary the walk never saw. It does not care how the escape was spelled.

A rest must not begin with a slash either, for a reason that is easy to miss.
`std::filesystem` discards the left-hand side of a join when the right-hand
side is absolute, so `/srv/static` joined with `/etc/passwd` is `/etc/passwd`.
The emptiness check every parameter already had is what prevents it, because a
rest beginning with `/` is a rest whose first piece was empty.

Empty segments inside a rest are kept, and so is a trailing slash.
Normalisation keeps them in the path, the rest reports the path as it stands,
and neither escapes a directory it is joined under: `/srv/static` joined with
`a//b` is still inside `/srv/static`. The guarantee is therefore exact rather
than tidy. A rest never begins with `/`, never holds a `.` or `..` segment,
and never holds a `/` that arrived escaped.

Single-segment captures keep decoding `%2F` into `/`. The two converters
promise different things: one hands over the text of a segment, which may
legitimately contain a slash, and the other hands over a location, which may
not gain one.

What a rest does not settle is everything that belongs to the file itself.
Whether the name exists, whether it is a symlink out of the tree, and whether
it is a directory are questions for whatever serves the file, and they cannot
be answered from the path alone.
