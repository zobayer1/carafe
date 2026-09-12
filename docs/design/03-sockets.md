# Sockets

Owning a descriptor, listening on a port, and reading and writing bytes. Part
of the [design notes](README.md).

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
