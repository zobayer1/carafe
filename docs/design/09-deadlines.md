# Deadlines

Bounding how long a client may hold a worker while silent, while sending, and
while being sent to. Part of the [design notes](README.md).

## A connection that says nothing still costs something

A thread per connection turned one slow client from an outage into a leak, which
is better and is not enough. Sixty sockets that each sent half a request line held
sixty threads and sixty descriptors for as long as the client cared to hold them,
and the server had no opinion about it at all: `recv` waits until bytes arrive or
the peer hangs up, and a peer that intends neither can wait for ever.

So every accepted socket gets `SO_RCVTIMEO`, and the plumbing for it already
existed. `Socket::read` reports the resulting `EAGAIN` as an `os_error` like any
other, `Connection` passes it up, and `serve_connection` already treated a read
failure as "stop, there is nobody to answer". The only new question was whether
stopping silently is right, and it is right exactly half the time.

A connection that finished its last request and went quiet is idle. Closing it is
routine, the client asked for nothing, and there is nothing to say. A connection
that got half a request out and stalled is a different thing: that client did ask,
and closing on it without a word leaves it unable to tell a refusal from a network
fault. RFC 9110 §15.5.9 has a status for precisely this, so it gets `408` and the
`connection: close` that goes with any response ending a connection.

Telling those apart looked like it needed the reader to expose its partial state,
and that would have been wrong. A client sending `GET /hel` and stopping leaves
`head_bytes_` at zero and the phase at `RequestLine`, because nothing has been
consumed yet; the bytes are sitting in `LineReader`. A predicate built on reader
state would answer "no request in progress" for the one case the whole feature
exists to catch. What actually knows is `Connection`, which sees both the bytes
arriving and the request being handed over, so the answer is one `bool` set on a
read and cleared by a completed request, and no new API below it.

Thirty seconds is arbitrary and nothing derives it. nginx splits the same idea
into sixty seconds for a header and seventy-five for an idle keep-alive; one
number for both is coarser and enough at this size. It is the sort of constant
that wants to become configuration long before it wants to become two constants.

Two holes are left open deliberately. The deadline is per `recv`, so a client
sending one byte every twenty seconds renews it for ever: bounding that needs a
deadline on the whole request rather than on each read, and the head cap bounds
bytes rather than time. And this is the receive side only, so a client that stops
reading a response can still block a thread in `send`, which is the same shape and
wants `SO_SNDTIMEO`. Both belong with the thread pool, because a pool without them
is worse than no pool: a bounded number of workers held by clients who never
finish is the original outage with a smaller number attached to it.

## A deadline that every byte renews is not a deadline

`SO_RCVTIMEO` bounds one `recv`, and that is not the same as bounding a request.
A client sending a single byte and waiting resets the clock with each one, so five
bytes over two minutes held a thread and a descriptor exactly as a silent client
had before the deadline existed. The head cap bounds the bytes, not the time: at a
byte every twenty seconds, sixty-four kilobytes is a fortnight away.

So the limit that matters is measured from the first byte of a request rather than
from the last, and the whole change is one `if`. Recording the deadline whenever
bytes arrive would compile, would pass every test worth writing for the stalled
client, and would preserve the bug exactly. It is recorded only on the transition
into "a request is arriving", which is the same transition the `408` already
turned on, so there is no second piece of state to keep in step.

Two limits, not one. Waiting for a request to *begin* should be generous, because
a client with nothing to say yet is an ordinary keep-alive connection. Waiting for
one to *finish* should not, because that is the only clock a client can wind back
by sending anything at all. They are separate fields for that reason, and the
tests keep only one of them short at a time so that neither can pass on the other
firing.

The deadline surfaces as two different errnos, and both are honest. If the client
stalls, the socket deadline fires first and `recv` reports `EAGAIN`. If the client
drips, reads keep returning bytes until the request deadline has passed and the
check ahead of the next read reports `ETIMEDOUT`. Which one arrives is a race
between the two, so nothing pins it; both mean the request ran out of time, and
both answer `408`.

Sub-millisecond remainders are the trap here. `SO_RCVTIMEO` reads a zero `timeval`
as *no deadline*, not as an immediate one, so truncating a remaining half
millisecond down would turn the last instant of a deadline into an unbounded wait.
Clamping to one millisecond fixes it and cannot be tested, since it needs a read
returning bytes inside the final microseconds; treating anything under a
millisecond as expired fixes it too, and is one guard instead of two.

Moving all of this behind the `Connection` constructor was worth more than it
looked. It made the request deadline testable in milliseconds, which it otherwise
was not at any speed. It also showed that three existing tests had been setting
`SO_RCVTIMEO` on the socket behind the server's back and were now being overridden
by the real deadline: they still passed, by waiting the full thirty seconds each,
and the suite went from one second to ninety-two. Tests that pass for the wrong
reason are quiet until something makes them loud.

The send side gets the same deadline for the same reason, since a client that
stops reading holds a thread just as well as one that stops writing.

## A short write is what the deadline was waiting for

`Socket::write` loops, because a partial send is an obligation to send the rest.
Bounding it with `SO_SNDTIMEO` bounds one `send`, and the loop then hands the next
one the whole limit again. That is the receive drip in the other direction, and it
was left standing when the request deadline landed.

It hides well, because a blocking `send` almost never comes back short. It does
not return once it has written what it can; it waits until the whole buffer is
queued. Two things cut it short: a signal, and its own deadline firing after it
has already moved bytes. The second is the one that matters, since the loop's own
bound is what sends the loop round again. Against a peer draining 64 KiB every
20 ms, a four megabyte write bounded at 100 ms took fifty one rounds and 5.15
seconds.

None of that is visible over a socketpair. `AF_UNIX` takes a whole payload into a
single `send`, so the loop runs once and a deadline on the loop cannot be told
apart from a deadline on the send. Reproducing it needs a loopback TCP pair with
`SO_SNDBUF` and `SO_RCVBUF` pinned small, which is the one place in the suite
where the transport has to be real. A first attempt at the test used a socketpair
and a peer sipping eight bytes at a time, and proved nothing twice over: the loop
never went round, and the sips were too small to wake the sender anyway, so the
per-send deadline answered for it.

So the deadline is fixed at the first send, and each round is handed what is left
of it. The clamp both sides need is now `milliseconds_until` in `net`, which
reports nothing when under a millisecond remains. The zero `timeval` rule is a
fact about sockets and reads better beside them than inside `Connection`.

`set_send_timeout` went with it. `write` sets `SO_SNDTIMEO` itself, once a round,
and a setter whose only remaining callers were its own tests is not an API. The
asymmetry with `set_receive_timeout`, which stays, is a real difference rather
than an oversight: `read` hands back one buffer per call, so the caller must loop
regardless, and only the caller knows when a request began. `write` is given the
whole response at once, so the deadline covers exactly one call and belongs
inside it.

The cost is one `setsockopt` per round. For a response that fits in the socket
buffers, which is nearly all of them, that is one extra syscall per response.

What is left is smaller and harder to see. Handing each round the remainder stops
a send from overshooting by more than the tail of one round, but a send starting
just before the deadline still runs to its own limit, so a write bounded at 100 ms
can take close to 200 ms in the worst case. That is the whole difference between
passing the remainder and passing the full limit, and no test pins it: reaching it
needs a send that begins in the last microseconds of the deadline.

The write deadline is still the request deadline. Pinning the expired branch on
the read side showed why that will not hold for ever, since a zero request
deadline leaves the response unwritable when the same number bounds the write.
Nothing asks for a zero deadline today, so the third deadline still waits, but it
has a second argument behind it now.
