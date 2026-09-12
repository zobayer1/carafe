# Bodies and persistence

Framing a body, refusing one without losing the stream, and deciding when a
connection ends. Part of the [design notes](README.md).

## A body is framed by the head, and by nothing else

`measure_body` takes a `const Headers&` and nothing more. Not the method, not
the version, not the target — the length of a body is decided by the fields that
declare it, and by no other part of the request.

That sounds like a tidiness rule and is really a bug fix. Before this, a `POST`
with a body was answered and its bytes left in the buffer, where the next
`next_line()` found them. A three-byte body in front of a pipelined `GET` made
`abcGET` parse as a method token, so a perfectly ordinary request came back
`501 Not Implemented` with `connection: close`. Not a 400 about the wrong
request — an actively wrong answer about the right one. Anything that reads a
body only for the methods it expects to carry one has the same hole, one verb
further along.

A request with no `Content-Length` has no body. RFC 9112 §6.3 rule 6 says so in
as many words, and the tempting alternative — a body running to end of stream —
is a rule for *responses* only, where the connection closing is what delimits
the content. Reading it into requests would mean a `POST` with no length field
consumed the rest of the connection, pipelining included. 411 is not the answer
here either: it is a status an origin server may choose when a handler requires
a length, not a framing rule, so it belongs to whatever handler wants it rather
than to the reader.

## One declared length, or none at all

Two spellings say a body is three bytes twice:

    Content-Length: 3
    Content-Length: 3

    Content-Length: 3, 3

RFC 9110 §5.3 lets a recipient fold repeated fields into one comma list, which
is exactly what makes those the same request. Both are refused even though the
values agree, because the risk was never disagreement between the two numbers —
it is disagreement between two *recipients*, one reading the pair as a length
and one rejecting it. That is how a second request gets smuggled inside the
first, and it costs nothing to be the recipient that refuses.

Neither rule catches both spellings. Counting fields catches the first and never
sees the second, which arrives as one field; requiring the whole value to be
`1*DIGIT` catches the second and says nothing about the first. A parser that
stopped at the first byte it could not use would read `3, 3` as 3 and frame the
request the way only one of the two disagreeing recipients would.

The cap is applied to the declared length, never to bytes already held. A
`Content-Length` over the limit is refused at the blank line, before a single
body byte is buffered, which is why `LineReader::take` carries no cap of its own:
by the time anything asks for bytes, the number has already been agreed to. It
is also why a twenty-digit length is a 413 rather than an overflow — the digit
loop stops as soon as the running value passes the cap, so it never reaches a
multiply that could wrap.

`Transfer-Encoding` is tested before any of this, and any value fails with a 501.
That ordering is not stylistic: it makes the `Transfer-Encoding` plus
`Content-Length` pair that §6.3 rule 3 singles out unreachable, rather than
something a further rule would have to catch. It also puts an unimplemented
coding on the far side of a line from an unimplemented method. Both answer 501,
but a method we do not serve arrives on a request we can measure and step over,
while a coding we cannot decode leaves us with no idea where the body ends. Same
status, opposite consequence for the connection.

## Refusing a request is not the same as losing the stream

Every rejected head used to be answered with `connection: close`, on the
reasoning that after a bad head the byte stream cannot be resynchronised. That
is true of most rejections and false of the one that matters. A body over the
size limit is refused on its declared `Content-Length` alone, before a single
byte of it is buffered — which means the length is *known*, and a reader that
knows the length knows exactly where the next request begins. Closing there
punishes a client for the one mistake we can recover from perfectly.

So `RequestResult` carries a `stream_continues` flag and `RequestReader` grew a
second way to fail. `fail` latches, so every later call repeats the failure;
`refuse` does not latch, arms a discard phase, and lets the reader step over the
body it declined. Not latching is the whole mechanism — there is no other switch.

The flag lives on the result rather than being computed from `RequestError`,
and the reason is visible at the two call sites that produce `BodyTooLarge`. One
comes from `measure_body`, where the declared length passed the drain ceiling or
was too large to hold at all; there is no number to count down, so the stream is
gone. The other comes from the cap comparison in `complete_head`, where the
length is known and modest; that one recovers. Same enumerator, opposite answers,
so no exhaustive `switch` over the enum could have expressed it. Recoverability
is a property of what the reader knows when it fails, not of the value it
reports — which is the same argument that keeps the 414-or-431 decision inside
`RequestReader` rather than in `LineReader`.

## Politeness has a ceiling

Draining is real work: skipping a declared body means reading and throwing away
every byte of it. `Content-Length: 999999999999` would have us do that
indefinitely to be courteous to a client that is not being courteous back. So
there is a second cap above the body limit, and past it a refusal closes after
all.

That ceiling is where `parse_content_length` stops counting, not the body limit,
and the difference is load-bearing. Bounding at the body limit would discard the
number for every length in between — exactly the lengths that are drainable —
and the drain would have nothing to count down. So the parser bounds at the
ceiling and returns a usable value; `complete_head` compares that value against
the body limit and decides. Grammar and absurdity in the parser, policy in the
reader.

The value itself is the one genuinely arbitrary constant here. Eight megabytes
buys back the connection for an upload a little over the limit and refuses to
spend the afternoon on one that is wildly over it. Nothing derives it.

## The client decides when the connection ends

Until now the server never read the `Connection` field. It answered a request,
went back to waiting for the next one, and let the client hang up when it was
finished. That is correct for exactly one kind of client: an HTTP/1.1 one that
said nothing.

RFC 9112 §9.3 puts the default in the version. HTTP/1.1 is persistent unless the
client says otherwise; HTTP/1.0 is not unless the client asks. An HTTP/1.0 client
learns that a response ended by watching the connection close, so holding the
socket open after answering one leaves it blocked on an end that never comes. It
recovers on its own timeout, seconds later, which is the whole difference between
a server that works and one that appears not to. A client that sent
`Connection: close` was ignored just as completely, though it recovers faster by
closing the connection itself.

The decision therefore has two inputs. The version supplies a default and the
field overrides it, and `close` is definitive: `Connection: keep-alive, close` is
a legal list and §9.3 makes the close win, which is why `client_wants_close` looks
for it before it consults the version at all.

Reading the field is fussier than it looks. §7.6.1 makes it a comma-separated list
of case-insensitive connection options, §5.3 lets a client spell one list across
several `Connection` fields and mean the same thing, and §5.6.1 puts optional
whitespace around every element. `Connection: closed` is an option nobody
implements rather than a `close` with a letter stuck on the end. Each of those is
a line of code and a test, and getting any of them wrong fails in the direction
that hangs a client rather than the direction that closes early.

§9.6 wants the response that ends a connection to say so. Without the field a
client cannot tell a deliberate end from a reply that was cut short, which is the
same ambiguity `content-length` exists to remove. Announcing and closing are one
decision, so `answer` performs both and returns whether the connection survived;
a caller cannot announce a close and then forget to make one, or close without
having said it.

The failure path is coarser on purpose. A failed parse hands over no headers, so
there is nothing to check for a `keep-alive`, and an HTTP/1.0 request that failed
is closed on whether or not it would have asked to stay. A server may always
close, so that sits inside §9.3; the opposite mistake leaves a client waiting.
That is the whole reason `RequestResult` carries a version out of a failure: it is
the one piece of the request that survives, and the only piece this decision
needs.

What did not change is `operator bool`. It answers whether the request parsed, and
a version is not a verdict on that — an HTTP/1.0 request that parsed cleanly is a
success. Persistence is settled after the response is written, on the success
branch and the failure branch alike, which is the same reason `stream_continues`
never belonged in the operator either.

One thing is deliberately left undone. A handler that puts `connection: close` on
its own response gets it serialized and then ignored: the connection stays open,
and if the client also asked, the field goes out twice. Letting a handler end a
connection is a real feature, but nothing needs it yet, and until something does
the framework owns the connection's lifetime by itself.

## A coding list is a decision table, not a feature flag

`Transfer-Encoding` used to be one line: if the field is present at all, answer
501 and close. That is a defensible placeholder and a bad rule, because the field
names a *list*, and RFC 9112 §6.1 and §6.3 give three different answers depending
on where in that list `chunked` sits.

If `chunked` is last, the body's end is findable, because each chunk says how long
it is and a zero-length one ends the sequence. If something else sits under it,
the end is still findable and the content is still undecodable, which is a 501
about the content rather than the framing. If `chunked` is anywhere but last, or
absent entirely, there is no way to know where the body stops, and that is a 400:
not "we did not implement this" but "this message cannot be read by anyone". And
`chunked` twice is the same answer for the same reason.

The pair rule sits above all of it. A message carrying both `Transfer-Encoding`
and `Content-Length` is refused before either is consulted, whatever the coding
is, because the danger is not in either field but in the disagreement: one
recipient frames by the length, the next frames by the coding, and the bytes in
between become a request only one of them can see. §6.3 rule 3 says 400 and close,
and closing is the part that matters.

RFC 9110 §5.3 makes the walk fussier than it looks. `Transfer-Encoding: gzip`
followed by `Transfer-Encoding: chunked` is the same list as
`Transfer-Encoding: gzip, chunked`, so every matching field has to be walked as
one, and a reader that stops at the first would conclude "chunked" from a list
that ends in something else. That is the same field-folding rule the `Connection`
work needed, which is why `next_list_element` exists in a header of its own rather
than twice in two files.

## The body is the thing that is never declared

A chunked body is the first one this reader handles whose length it is never
told. Everything the `Content-Length` work established has to be re-derived from
nothing: the size cap, the overflow bound, the point at which a refusal happens.

The cap moved from a single test to a running one. There is no total to compare
against, so each chunk is added to what has already arrived and the sum is checked
before the bytes are taken. The overflow bound moved too, and it is the subtler
of the two. `parse_chunk_size` stops counting at the body cap not because that is
where the refusal belongs, but because a size line may carry 8192 hex digits and
`0x10000000000000000` wraps to zero on a 64-bit `size_t`. A wrapped size does not
produce a wrong error; it produces a *plausible* one. Zero ends the body early,
and a size that wraps to five frames a five-byte chunk and leaves the rest of the
body to be parsed as the next request. The cumulative check cannot catch either,
because by the time it runs the number is already small.

Chunk-size lines are deliberately not charged to the head cap. They are framing
for the body, and counting them there rejected a 22 KB upload, a fiftieth of what
the body cap allows, purely for arriving in one-byte pieces, and told the client
its header fields were too large. The same reasoning gives an over-long chunk-size
line a 400 rather than the 431 the head phases report: only the reader knows which
line it asked for.

The trailer section is read and dropped. Keeping it would be a small feature and a
real hazard: the head has already been validated by the time a trailer arrives, so
a field admitted there is a field that arrived somewhere nothing checks it. It is
still parsed before being discarded, because a trailer section that is not field
lines is a framing error like any other, and swallowing arbitrary bytes until a
blank line is exactly the sort of hole a smuggled request fits through.

Two limits are deliberate rather than discovered. An oversized chunked body
fails and closes instead of draining, unlike a declared one, which is a step
back from [Refusing a request is not the same as losing the
stream](#refusing-a-request-is-not-the-same-as-losing-the-stream): draining is
possible here, since the sizes are self-describing, but it needs its own state
and the ceiling reapplied per chunk. And framing overhead is bounded only
indirectly. Every chunk carries at least one byte, so a 1 MiB cap admits at
most a million chunks, but a client sending them one byte at a time makes us
read about six megabytes to get there. Bounded, not tight.
