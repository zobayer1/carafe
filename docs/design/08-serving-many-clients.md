# Serving many clients

Threads, the accept loop that must outlive its failures, and the bounded pool.
Part of the [design notes](README.md).

## One slow client was every client

Serving each connection to completion before accepting the next is the simplest
thing that works, and it worked for as long as connections were short. Keep-alive
ended that. A client that asks once and then holds its connection open is doing
exactly what HTTP/1.1 tells it to do, and it took the whole server with it: a
second client got nothing at all, indefinitely, and a third queued behind that.
No malice and no request were required. Holding the socket was enough.

So each accepted connection now gets a thread, and the thread is detached. Not
joined, because there is nothing to join it to: the accept loop returns only when
accepting itself fails, and a loop that paused to reap finished threads would be
choosing between doing that and accepting.

Detaching creates the problem worth writing down. A detached thread can outlive
the call that spawned it, and `run()` does return, on any accept failure that
retrying would not fix. If the `App` is destroyed at that point, every thread
still serving is holding a reference to a `Router` that no longer exists. The
answer is not to document the hazard but to remove it: `router_` became a
`shared_ptr`, and each thread captures its own copy, so the routing table lives
until the last connection using it is finished. Nothing has to be sequenced and
nothing has to be remembered.

The copy each thread holds is a `shared_ptr<const Router>`, and the `const` is
load-bearing rather than decorative. Threads read; registration writes. That was
always the intended usage, and `run()` already asked for it in a comment, but
until now registering a route mid-flight was merely surprising. With connections
overlapping it is a data race on a `vector` that live threads are walking, which
is a different class of wrong.

Spawning a thread can fail, and `std::thread`'s constructor reports that by
throwing. Left alone it would terminate the process, which is the one outcome
worse than the problem being solved: every connection already in flight dies with
it. So the loop catches, drops that one client, and carries on. The client sees a
reset and can retry. Serving it inline instead would have been the tempting
alternative and reintroduces the outage exactly when the machine is least able to
afford it.

Two things this does not fix. There is still no idle timeout, so a client that
connects and says nothing holds a thread until it hangs up. And nothing bounds
how many threads exist; the ceiling is whatever the process descriptor limit
allows. Both are real, and both are leaks rather than outages, which is the whole
difference: one bad client now costs a thread instead of the server. A pool
bounds the count and a receive deadline bounds the wait, and they belong together
in the milestone after this one.

Testing it needed one compromise. `serve_forever` returns only when accepting
fails, so a test cannot ask it to stop without `Listener` growing a `close()`
whose only caller would be that test. The test detaches its server thread instead
and lets it park in `accept()` for the life of the process. The listener moves
into the thread rather than being leaked beside it, so nothing outside outlives
what the thread is still using. The concurrency itself is asserted without
measuring a clock: eight handlers have to be inside the server simultaneously
before any of them may return, which one thread cannot arrange however long it is
given.

## Only the listener failing may end the server

The accept loop used to treat one errno as routine and every other as final:
`ECONNABORTED` meant a queued connection had hung up, and anything else meant the
listener was finished. That reading is wrong about most of the list, and it is
wrong in the direction that ends the process.

`accept` reports three quite different kinds of thing through one channel. Some
errors belong to the listening socket: `EBADF`, `EINVAL`, `ENOTSOCK`. Those are
final, and retrying asks the same question forever. Some belong to the connection
being accepted, and `accept(2)` is explicit that network errors for the new socket
surface here, so `EPROTO`, `ETIMEDOUT` and the unreachable family are one client's
bad luck. And some belong to the moment rather than to anything: `EMFILE`,
`ENFILE`, `ENOMEM`, `ENOBUFS` say the kernel is out of something it will have
again once a connection finishes.

Only the first kind may end the server. That is the rule, and it is worth stating
as a rule because the old code had the right instinct about `ECONNABORTED` and
generalised it in the wrong direction. Sixty connections that each sent half a
request line, against a descriptor budget of sixty-four, were enough to make the
process exit. No requests, no malice past opening sockets, and afterwards nothing
answered at all.

The two survivable kinds want different treatment, which is why the answer is a
three-valued enum rather than a bool. A connection that failed costs nothing to
skip, so the loop asks again at once. Exhaustion costs a core if asked again at
once, because the answer only changes when a thread somewhere finishes, so that
one waits ten milliseconds first. A bool would have needed a second test naming
`ECONNABORTED` again to tell those apart, and that test would have been the only
thing recording the distinction.

This is also the one place a `switch` here takes a `default`, against the habit of
the rest of the tree. Elsewhere the missing default is what forces a new
enumerator to be classified. `errno` is not an enum, nothing will announce a new
value, and the safe answer for one nobody has considered is to stop rather than to
spin quietly forever.

What this does not do is make exhaustion harmless. The server survives and cannot
serve anyone until the descriptors come back, and they come back only when the
clients holding them relent. Making that recovery independent of the attacker is
the idle timeout, not this.

## A bounded pool is the outage again with a number on it

A thread per connection has no ceiling of its own. It borrows one, and the one it
borrows is the process descriptor limit, which nothing here chose. A pool replaces
that with a number the server picks.

The ordering mattered more than the pool did. A bounded pool without deadlines is
the original one-slow-client outage with a smaller number attached: sixty four
clients that say nothing hold sixty four workers for as long as they care to, and
everyone else waits. That is why the deadlines went in first and why the pool
takes them as a constructor argument, so every connection it builds is bounded by
the same policy.

One worker serves a whole connection rather than one request. A keep-alive client
therefore holds its worker until it goes away or a deadline fires, so a full pool
does not mean requests are queueing behind each other. It means no new client is
served until an existing one ends. That is the head of the line blocking the rest
of it, and the queue deadline exists to bound how long anyone waits in it: a
connection that sat there longer than the limit is dropped when a worker finally
reaches it, because a client that gave up is a worker spent on nobody.

The queue is much larger than the worker count on purpose. Workers bound what is
being served; the queue absorbs the arrivals that land while they are busy. Past
the queue there is nothing left to do but close the connection as it arrives. A
`503` would be the better answer, but the send would happen on the accept thread,
and blocking there is the outage three commits went into removing. There is no
non-blocking write to do it with, so rejection is a close, and the status waits
for one.

Two things about stopping are easy to get backwards. Queued connections are
dropped rather than worked through, because a server that is going down owes
nobody a reply it will not be around to follow. And a worker already inside a
connection finishes it, so joining can take as long as one live connection lasts.
Both are worth knowing before writing a test: three of the pool tests failed on
the first run because they submitted a connection and destroyed the pool in the
next statement, and stopping dropped it before any worker got there.

Starting the workers has one hazard. A constructor that throws has no destructor,
so a `std::system_error` from the fifth thread would leave four running with a
vector destroyed under them, which is a call to `std::terminate`. Failing to start
a worker is treated the way a failed accept is: keep what is running, serve with a
smaller pool than asked for, and carry on.

## A number the caller cannot reach is the library's number, not theirs

The pool landed with its bounds written into it. Sixty four workers, a queue
of five hundred and twelve, thirty seconds on each deadline: all reasonable,
none of them the caller's. `App::run` took a port and nothing else, so the
only way to serve with different numbers was to skip `App` and call
`serve_forever`, which is the internal API. A default nobody can change is not
a default.

`Deadlines` and `PoolLimits` moved out of `src/server/` into
`include/carafe/config.hpp` rather than being mirrored by a public pair of
structs with a translation function between them. Mirroring means the same
five defaults written twice, and they drift the first time one of them moves.
The move costs nothing at the use sites: internal code names them unqualified
from inside `carafe::server`, and lookup finds them in the enclosing
namespace.

Zero is refused rather than honoured. Every field has a zero that reads like
*unlimited* and behaves like the opposite. No workers means nothing ever takes
from the queue. No queue room means every arrival is closed even while every
worker sits idle, because a worker is only ever handed work through the queue.
A zero deadline is the trap that cost a whole afternoon earlier: it reaches
`setsockopt` as no deadline at all, so it does not fire immediately, it never
fires. A caller who wants no limit passes a large value and means it.

Refusing brought a second problem with it. `run` returned `bool`, which was
honest while the only failure was a port that would not bind, and a lie the
moment there were three. The example printed `could not serve on port 8080`
for a rejected deadline, which sends the reader to the one thing that was not
wrong. `RunError` names each one, and `describe` keeps the wording in the
library rather than in every caller. It has no `None`: `run` returns only when
it has given up, so a success value would be a state that never exists.

The checks sit in front of the bind, which is what makes them observable. A
test holds a port, asks `run` to use it with a zero worker count, and requires
`InvalidLimits` rather than `BindFailed`. Had the check gone missing, `run`
would reach the bind and report that instead, so the test fails rather than
passing on a technicality.

## One handler that throws was every client

A handler is the caller's code, and nothing stood between what it threw and
the worker thread it ran on. An exception escaping a thread's function calls
`std::terminate`, so one handler throwing ended the process, and every
connection another client held went with it. Measured before the fix: a
request to a throwing route came back empty, a bystander answered a moment
earlier found its keep-alive connection closed, the next client was refused,
and the server exited with status 134.

The handler call is now wrapped, and anything it throws becomes a `500`. The
catch is `catch (...)` rather than `std::exception`, because a handler is
arbitrary code and may throw anything at all, an `int` included. The body is
the status and its phrase and nothing more: an exception's message is for
whoever runs the server, and sending it to the client leaks internals. The
connection stays open, for the same reason a `404` keeps it: the request was
read to its end, so the stream is still in step. The helper is not `noexcept`,
since building the `500` allocates, and a `noexcept` would turn a failed
allocation into exactly the crash being removed.

This belongs in the server rather than in optional middleware. A guarantee a
caller has to remember to install protects nobody who forgot. Middleware
inherits it instead, provided the catch wraps whatever calls the handler
rather than the handler call alone.

Only the handler is covered. Routing, the `404` and `405` builders, the reader
and serialisation are library code, and an allocation failure in any of them
still ends the process. A last-resort catch in the pool worker would reduce
that to one lost connection, but nothing can make those failures happen on
demand, so it would ship untested, and it is left out on those grounds.

## Refusing costs a send that cannot wait

Rejection was a close, and the note above says why: the send happens on the
accept thread, and a thread that waits for one client is not accepting for any
of them. The missing piece was a write that reports instead of waiting.

`Socket::write_now` is that write. It sends once with `MSG_DONTWAIT` and
returns `EAGAIN` where the socket has no room, rather than blocking. The flag
belongs to the call, not to the descriptor, which is why it needs no `fcntl`
and leaves nothing behind for the next writer. It also beats anything an
earlier write left on the socket: measured against a filled socket carrying a
five second send timeout, the flagged call returned `EAGAIN` in a tenth of a
millisecond, and the same call without it waited 5.4 seconds for the same
answer. A send that placed only some of the bytes is reported as `EAGAIN` too,
since the caller is closing either way and a count would tell it nothing it
could act on.

The refusal is serialized once, when the pool is built. It never varies, and
building a response, adding two fields and serializing it would be work done
on the accept thread at the exact moment a flood is arriving. Refusing is now
one send of bytes the pool already holds.

Both rejections answer the same way. A full queue is refused on the accept
thread, and a connection that waited past `queue_wait` is refused by the
worker that reaches it. The worker could afford a bounded blocking write, and
does not take it: that client is past its own deadline and most likely gone,
so waiting on it would spend the worker on nobody, which is what dropping it
was for.

What a client actually sees was worth measuring rather than assuming. The pool
never reads the request, so it closes a socket with unread bytes in it, which
sends a reset. The refusal still arrives: it was already in the client's
buffer before the close, and both a TCP connection and a socketpair delivered
it in full. The difference shows up on the read after: a client that had sent
a request gets a reset there, one that sent nothing gets a clean end of
stream.

No `Retry-After`. RFC 9110 §15.6.4 allows it, and the pool has no idea when a
worker frees up: `queue_wait` bounds how long a queued connection may wait,
not how long this refused one should stay away. A number made up to fill the
field would be worse than the field's absence.

## The last exception a worker can meet

A handler that throws became a `500` two commits ago, and everything else on a
worker's path is this library: the reader taking in a head and a body,
routing, serialising a response, writing it. An allocation failure in any of
them left the worker's function, and an exception leaving a thread's function
calls `std::terminate`. One `bad_alloc` in the wrong place ended the process,
which is exactly the outage a throwing handler used to cause.

The worker now wraps the whole of one connection, building it included, and
drops it on anything that escapes. There is nothing to answer with: composing
a response allocates too, and the failure at hand was an allocation. So the
connection closes, the worker takes the next, and the process keeps serving
everyone else. That is the entire ambition.

This was left out of the earlier commit as untestable, which was true of
everything tried at the time and not true in general. A test can replace
global `operator new` for the test binary and arm it to fail exactly one
allocation at or above a chosen size, disarming itself the moment it fires.
Inert until armed, it changes nothing for the other tests.

A sanitizer build is the exception. It installs an allocator of its own and
checks that every allocation is released through the operator that matches it,
and gtest takes the `nothrow` form of `operator new` before the first test
runs. Replacing only the ordinary form breaks that pairing, and replacing the
whole family would blunt the same checking for the library, which is what the
build is for. So the device is compiled out there and the test skips, which
leaves the sanitizers looking at the library rather than at a device the tests
brought with them.

The failure is aimed at the reader, growing its buffer while it takes in a
body. That is the one allocation a test can provoke which sits outside every
catch already in place: a handler cannot stand in, because a handler's throw
is a `500` long before the worker sees it. The assertion that matters is the
second connection, served normally afterwards, which is the worker having
outlived the failure rather than the connection having survived it.

One exposure of the same kind remains. The accept thread allocates when it
queues a connection, and a failure there still ends the process. It wants the
same treatment and a way to aim a failure at it, which is a commit of its own.
