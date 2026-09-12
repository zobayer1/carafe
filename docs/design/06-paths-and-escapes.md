# Paths and escapes

What a path means before any route sees it: percent-escapes and normalisation.
Part of the [design notes](README.md).

## Splitting comes first, decoding second

A capture is percent-decoded, and it is decoded *after* the path has been cut
into segments, never before. RFC 3986 §2.2 makes a percent-encoded octet data
*within* a segment, so `%2F` is a slash the client wants inside one parameter,
not a separator between two. Decoding the path first would turn `/users/a%2Fb`
into three segments and `/users/<id>` would never see `a/b`.

The same order matters one layer up: `path_of` cuts the target on a raw `?`
before anything is decoded, so `%3F` stays inside the segment instead of ending
the path. And the order is not only about correctness of matching — decode-first
is exactly how path traversal gets in, because `%2E%2E%2F` becomes `../` before
anything has a chance to reason about segments. Static file serving is on the
roadmap, and getting this order wrong now would be a hole waiting for it. Two
tests exist to forbid the other order rather than to check the happy path.

`+` is not decoded to a space. That rule belongs to
`application/x-www-form-urlencoded`, which describes query strings and form
bodies. A path segment is neither, and Flask does not decode it there either.

Only captures are decoded. A literal segment is still compared as the bytes it
was registered with, so a route added as `/café` does not answer `/caf%C3%A9`
even though they name the same resource. Making it would mean normalising both
sides, which drags in dot-segments, case, and Unicode normalisation — a commit
of its own. The gap is a test rather than a comment, so it announces itself the
day someone closes it.

## A precondition nobody can check is a precondition worth handling

`parse_request_line` rejects a target whose percent-escapes are not `"%" HEXDIG
HEXDIG` with a 400, which means the router's decoder never meets an escape that
does not decode. The tempting conclusion is that the decoder can therefore skip
the check — and that was the original plan, written into a comment before it was
written into code.

It is wrong, and not by a small margin. `Router::find` takes a `std::string_view
target` and nothing in that type records that the bytes came through the parser;
`router_test.cpp` already calls `find` directly, and the next caller will too. A
decoder that assumed two hex digits were present reads `text[i + 2]` for a target
ending in `%4`, which is a read past the end of the view — not a wrong answer, an
out-of-bounds one. Since the bounds test is not optional, checking the two digits
alongside it costs nothing and gives one rule instead of two: an escape that is
not `"%" HEXDIG HEXDIG` is copied as it stands.

That fallback is unreachable through a served request, which is a reason to
document it on `find`, not a reason to leave it out. The alternative — an error
channel on `find` — was rejected for the reason it has always been rejected here:
`add` and `find` have no way to report, and giving them one to serve a case the
parser already answers would be a large change bought with nothing.

## Three spellings of one path were three routes

A target used to reach the router exactly as it arrived. `/a/b`, `/a/./b` and
`/a/x/../b` name one resource and matched three different things, which is two
bugs wearing one coat. The dull half is that a client spelling a path the long
way round gets a 404. The sharp half is that `..` reaches a capture, and a
capture is what a file handler is going to open.

Normalisation runs in two passes and the order is the whole point. The first
resolves percent-escapes, the second removes dot segments. Doing it the other
way round leaves `%2e%2e` looking like an ordinary name, so it survives the
walk and arrives as a capture holding `..`.

The first pass resolves only the escapes of unreserved characters, which RFC
3986 §2.3 defines and §6.2.2.2 says are the only ones equivalent to the
character they stand for. That single rule does two jobs. `%2e` becomes `.`,
so an encoded dot segment cannot hide from the second pass. `%2f` stays `%2F`,
so no client can spell a separator that the split will not see. Everything
else keeps its escape with the hex uppercased, because two spellings of one
byte should not route to two places.

It also settled an old question. A literal segment used to be compared as the
bytes it was registered with, recorded as a known gap that normalisation would
close. It closed half of it: `/t%65a` now reaches a route registered at
`/tea`, while `/caf%C3%A9` still does not reach one registered at the raw
UTF-8 bytes. That half is not a gap any more, it is the RFC declining to grant
the equivalence for anything reserved.

Resolving escapes before matching and decoding captures after it looks like
decoding twice, and is not, for the same reason. `%` is not unreserved, so
`%25` survives the first pass and only the capture decode turns it back into a
percent sign. A handler asking for `/echo/%2541` is given the text `%41`, and
one asking for `/echo/%252e%252e` is given `%2e%2e`. A second decode there
would hand back the traversal the first pass had just removed.

Both sides are normalised, because normalising only the target moves the
mismatch rather than removing it: a pattern registered as `/a/./b` would still
sit there unreachable. The pattern gets it at registration, where it is paid
for once, beside the split that is already done there for the same reason. A
parameter segment passes through untouched, since the walk only acts on a
segment that is exactly `.` or `..`, and `<..>` is neither.

Two things are deliberately left alone. Empty segments stay, so `//a` is not
`/a`: the RFC does not merge them, and a pattern cannot match one, so the
surprise is a clean miss rather than a path that quietly means two things. A
trailing slash stays significant, so `/hello/` will not match `/hello`.
Merging those would make a route answer a path it never claimed. The
friendlier alternative is a 308 redirect to the canonical form, which is a
feature with a response path of its own.

The query is cut before any of this runs, and the order there is not cosmetic.
Normalising first turns `/a/b?x=../../y` into `/a/y`, which is a query string
rewriting the path it was attached to.
