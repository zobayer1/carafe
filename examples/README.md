# Examples

`make run` builds and starts `examples/hello.cpp` on `http://localhost:8080`. It
registers enough routes to drive every framing path by hand — a static route, a
path parameter, two bodies, and one path answered under three verbs.

```cpp
carafe::App app;

app.get("/hello/<name>", [](const carafe::http::Request& request) {
    std::string body = "hello, ";
    body += request.params.get("name").value_or("world");
    return carafe::http::text_response(200, body + "!\n");
});

app.post("/echo", [](const carafe::http::Request& request) {
    return carafe::http::text_response(200, request.body);
});

app.put("/store/<key>", [](const carafe::http::Request& request) {
    std::string body{request.params.get("key").value_or("?")};
    return carafe::http::text_response(200, body + " = " + request.body + "\n");
});

const carafe::RunError stopped = app.run(8080);
```

## Registered routes

| Method | Path             | Answers with                                  |
| ------ | ---------------- | --------------------------------------------- |
| GET    | `/`              | the version and a hint                        |
| GET    | `/hello`         | the request target it was asked for           |
| GET    | `/hello/<name>`  | a greeting using the captured segment         |
| GET    | `/users/<int:id>` | the captured id, for a segment of digits     |
| GET    | `/users/<name>`  | the captured name, for anything else          |
| GET    | `/files/<path:rest>` | the sub-path it was given, read from nowhere |
| POST   | `/echo`          | the request body, unchanged                   |
| POST   | `/size`          | the body's length in bytes                    |
| PUT    | `/store/<key>`   | the captured key and the body together, given the token |
| PATCH  | `/store/<key>`   | the same, to show a second body verb frames alike, given the token |
| DELETE | `/store/<key>`   | the captured key alone, given the token |
| OPTIONS| `/store/<key>`   | an `allow:` list, registered through `route()` |

`get`, `post`, `put`, `patch` and `del` are named helpers; `del` is spelt short
because `delete` is a keyword. Anything else goes through `route()`:

```cpp
if (!app.route(carafe::http::Method::Options, "/store/<key>", handler)) {
    // Head and Connect are the only two it refuses.
}
```

`route()` answers `false` for `Head` and `Connect` rather than registering them.
`HEAD` is served by the `GET` on the same path, with the headers a `GET` would
have sent and none of the bytes — a hand-written HEAD route would have to
reproduce those headers itself, and `serialize` would compute `content-length`
from whatever body the handler returned. A `CONNECT` target is an authority such
as `example.com:443` rather than a path, so a route registered at one could never
match. The named helpers return `void` because their method is fixed and cannot
be refused.

Four verbs share `/store/<key>`, which is what makes a wrong-method request
there show a real list:

```console
$ curl -si -X POST http://localhost:8080/store/k | grep -Ei 'HTTP/|^allow'
HTTP/1.1 405 Method Not Allowed
allow: PUT, PATCH, DELETE, OPTIONS
```

## Trying it

```sh
curl -i http://localhost:8080/hello/world
curl -i --data 'hi there' http://localhost:8080/echo
curl -i -X PUT -H 'Authorization: Bearer letmein' --data 'v' http://localhost:8080/store/k
curl -i -X PATCH -H 'Authorization: Bearer letmein' --data 'more' http://localhost:8080/store/k
curl -i -X DELETE -H 'Authorization: Bearer letmein' http://localhost:8080/store/k
curl -i -X OPTIONS http://localhost:8080/store/k
curl -i -X POST http://localhost:8080/hello          # 405, with allow:
curl -i http://localhost:8080/missing                # 404
```

A percent-escape in a captured segment is decoded, and only within that segment:

```sh
curl -i http://localhost:8080/hello/a%2Fb            # hello, a/b!
curl -i --path-as-is http://localhost:8080/hello/a%25b  # hello, a%b!
```

## Saying what a segment may be

`<name>` captures one segment and takes whatever is in it. `<int:name>` takes
only a segment of ASCII digits, and `<str:name>` is the long spelling of the
default. The example registers both forms on the same shape of path:

```cpp
app.get("/users/<int:id>", ...);
app.get("/users/<name>", ...);
```

```console
/users/42          user #42
/users/bob         user named bob
/users/007         user #007
/users/4a          user named 4a
/users/%34%32      user #42
/users/            404 Not Found
```

Order is the thing to know. The first pattern that matches wins and a
converter buys no precedence, so registering `/users/<name>` first would take
`/users/42` with it and the typed route would never answer. It is registered
first on purpose.

Row four is what "no match" means here. `4a` is not digits, so the typed
pattern does not match and the next one does; there is no falling back inside
a route. Row five is normalisation arriving first, since digits are unreserved
and `%34%32` has already become `42` before the router looks at it. Row six is
the rule that a parameter stands for something, which an empty segment does
not.

A converter constrains matching alone. The capture is still text, so `007`
keeps its zeros, and a handler wanting a number parses it.

An unknown converter is not an error, because `add` has no channel to report
one on. A route registered at `/users/<integer:id>` is a literal path spelled
exactly that, which no request will ever ask for.

## A parameter for the rest of the path

`<path:rest>` takes every segment that is left, so one route stands for a
whole subtree. The example echoes the sub-path it was given and reads nothing
from disk, because the capture is what a file handler would join under its
root:

```console
/files/css/site.css          you asked for the file at css/site.css
/files/a                     you asked for the file at a
/files/a//b                  you asked for the file at a//b
/files/a/                    you asked for the file at a/
/files/x/../css/site.css     you asked for the file at css/site.css
/files/a%20b/c               you asked for the file at a b/c
/files/a?next=/etc/passwd    you asked for the file at a
/files/a%2Fb                 404 Not Found
/files/a%2F..%2Fb            404 Not Found
/files//etc/passwd           404 Not Found
/files/                      404 Not Found
```

The first seven rows answer. Row five is normalisation arriving first: the dot
segment is gone before the router looks, so a rest never holds one. Rows three
and four keep an empty segment and a trailing slash, because the path has them
and neither leads anywhere a join would leave its root. Row seven leaves a
query that looks like a path in the query.

The last four rows are refusals, and each reason is about what a file handler
will do with the value. Rows eight and nine would decode to a separator the
client sent escaped, and row nine would put back `a/../b`, the traversal
normalisation had removed. Row ten would begin with `/`, and `std::filesystem`
throws the root away when the right-hand side of a join is absolute. Row
eleven has nothing to capture.

A rest has to be the last segment of its pattern. One registered as
`/a/<path:x>/b` is accepted and simply never matches, since the rest leaves
nothing for the segment after it.

So a rest never begins with `/`, never holds a `.` or `..` segment, and never
holds a `/` that arrived escaped. Whether the file exists, or is a symlink out
of the tree, is still for the handler to check.

## One path, one spelling

A target is normalised before it is matched, and so is the pattern it is matched
against, so a path spelled the long way round reaches the route it names. Every
row below is the first line of the response body:

```console
$ # curl rewrites a path itself, so --path-as-is is what sends these as written
/hello/world               hello, world!
/hello/../hello/world      hello, world!
/hello/./world             hello, world!
/../../../hello/world      hello, world!
/hello/..                  carafe 0.1.0
/hello/%2e%2e              carafe 0.1.0
/hello/a%2Fb               hello, a/b!
/hello/%252e%252e          hello, %2e%2e!
```

Rows five and six are the ones that matter. `..` pops the segment before it, so
both name the root and are answered by the route registered at `/`. Neither
reaches `/hello/<name>`, so no handler is ever handed `..` as a capture.
Row six is the same thing spelled in hex: escapes are resolved before segments
are split, so an encoded dot segment cannot hide from the walk.

Row seven is the other half of that order. An escaped separator stays escaped
through normalisation and is decoded only once the segment is a capture, so it
is one byte of a name rather than the end of one. Row eight keeps those two
rules from cancelling out: `%` is not an unreserved character, so `%25` survives
normalisation and only the capture decode turns it back. A second decode there
would hand `..` straight back.

RFC 3986 §6.2.2.2 is why only some escapes are resolved. `%65` and `e` name
the same path, so `/t%65a` would reach a route registered at `/tea`. `%C3%A9`
and the bytes it stands for do not, so those stay apart.

A trailing slash is part of the path, so `/store/k/` is not `/store/k` and comes
back `404`. Nothing here redirects one to the other.

## Middleware

Two middleware sit in front of every route. The first writes an access log
line for each request once it has been answered. The second refuses a write to
the store that does not carry the token, and never lets it reach a handler:

```cpp
app.use([](const Request& request, const carafe::http::Next& next) {
    const bool writes =
        request.method == Method::Put || request.method == Method::Patch || request.method == Method::Delete;
    if (writes && request.headers.get("authorization") != "Bearer letmein") {
        carafe::http::Response refused = carafe::http::status_response(401);
        refused.headers.add({"www-authenticate", R"(Bearer realm="carafe")"});
        return refused;
    }
    return next(request);
});
```

Returning without calling `next` is the whole refusal. RFC 9110 §15.5.2 makes
`www-authenticate` a MUST on a `401`, which is why it is there:

```console
$ curl -si -X PUT --data 'v' http://localhost:8080/store/k
HTTP/1.1 401 Unauthorized
content-type: text/plain; charset=utf-8
www-authenticate: Bearer realm="carafe"
content-length: 17

401 Unauthorized
```

| Request                                   | Status |
| ----------------------------------------- | ------ |
| `PUT /store/k` with the token             | 200    |
| `PUT /store/k` without it                 | 401    |
| `PATCH /store/k` without it               | 401    |
| `DELETE /store/k` with the wrong token    | 401    |
| `DELETE /store/k` with the token          | 200    |
| `GET /hello/world`                        | 200    |
| `POST /echo`                              | 200    |
| `OPTIONS /store/k`                        | 200    |
| `GET /missing`                            | 404    |
| `HEAD /hello`                             | 200    |

The check is on the method, not the path, and that is deliberate. A middleware
sees the target as the client sent it, while the router matches the normalised
path, so a check for an `/admin` prefix is walked around by
`/x/../admin/secret`. Guarding the three store writes leaves `GET`, `POST` and
`OPTIONS` open, as the table shows.

The same requests, as the server logged them with its stdout going to a file:

```console
$ ./build/debug/bin/hello > access.log
PUT /store/k 200 6B 2us
PUT /store/k 401 17B 2us
PATCH /store/k 401 17B 2us
DELETE /store/k 401 17B 4us
DELETE /store/k 200 10B 6us
GET /hello/world 200 14B 2us
POST /echo 200 2B 4us
OPTIONS /store/k 200 0B 2us
GET /missing 404 14B 1us
HEAD /hello 200 21B 3us
```

The log is registered first, so it runs outermost and records what the auth
middleware refused: the `401`s are there with their 17-byte bodies. So is the
`404`, because middleware wraps every request that parsed, not only the ones a
route claimed.

`HEAD /hello` is logged at 21 bytes although none are sent. The log sees the
`GET` handler's whole response, and the body is only dropped later, when the
connection writes it, which is how a `HEAD` still reports the length of the
`GET` body. The time covers the inner middleware and the handler, not routing
and not the network.

Each line is flushed as it is written. To a terminal that changes nothing,
since stdout is line buffered there. To a file or a pipe it is block buffered:
a reduced copy of this log without the flush got none of five lines into the
file while the server ran, and none were there after it stopped. With the
flush, every line above was in the file before the server was stopped.

## How long the connection lives

Every row is one connection carrying a single `GET /hello`. The question is what
the server does once it has answered.

| Request                                | After the response                     |
| -------------------------------------- | -------------------------------------- |
| `HTTP/1.1`, no `Connection` field       | held open                              |
| `HTTP/1.1` with `Connection: close`     | closed, and the response says so       |
| `HTTP/1.0`, no `Connection` field       | closed, and the response says so       |
| `HTTP/1.0` with `Connection: keep-alive`| held open                              |

RFC 9112 §9.3 supplies the defaults in rows one and three: HTTP/1.1 is persistent
unless told otherwise, HTTP/1.0 is not unless asked. The `Connection` field
overrides either. `close` wins whenever it appears, so `Connection: keep-alive,
close` closes.

Rows two and three are the ones that used to hang. The server never read the
field at all and held every connection open, which is right for exactly one
client: an HTTP/1.1 one that said nothing. An HTTP/1.0 client learns the response
ended by seeing the connection close, so it sat waiting for an end that was never
coming, and a client that had explicitly asked to close was ignored.

Where the connection is closed, the response carries `connection: close` before
the bytes stop, per RFC 9112 §9.6. Without it a client cannot tell a deliberate
end from a reply that was cut short.

```sh
curl -i --http1.0 http://localhost:8080/hello
curl -i -H 'Connection: close' http://localhost:8080/hello
```

`curl` closes the connection itself either way, so it will not show you the
difference. This does:

```python
import socket

def probe(label, request):
    s = socket.create_connection(("localhost", 8080)); s.settimeout(2)
    s.sendall(request.encode())
    out = b""
    try:
        while True:
            chunk = s.recv(65536)
            if not chunk:
                break
            out += chunk
    except socket.timeout:
        print(f"{label:<34} held open"); s.close(); return
    s.close()
    said = "connection: close" in out.decode(errors="replace").lower()
    print(f"{label:<34} closed, announced: {said}")

probe("1.1, nothing said", "GET /hello HTTP/1.1\r\nHost: x\r\n\r\n")
probe("1.1, Connection: close", "GET /hello HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n")
probe("1.0, nothing said", "GET /hello HTTP/1.0\r\nHost: x\r\n\r\n")
probe("1.0, Connection: keep-alive", "GET /hello HTTP/1.0\r\nHost: x\r\nConnection: keep-alive\r\n\r\n")
```

A failure the reader cannot resume from closes regardless of any of this: there
is no way to find where the next request begins. A refusal it *can* read past,
such as the oversized body below, keeps an HTTP/1.1 connection open and closes an
HTTP/1.0 one, because a failure hands over no headers to check for a
`keep-alive`.

## A body whose length is never declared

`curl` sends a chunked body whenever it is uploading something it cannot measure
in advance, which is any stream:

```sh
printf 'hello world' | curl -i -X POST --data-binary @- \
    -H 'Transfer-Encoding: chunked' http://localhost:8080/echo
head -c 100000 /dev/zero | tr '\0' 'x' | curl -s -X POST --data-binary @- \
    -H 'Transfer-Encoding: chunked' http://localhost:8080/size
```

Each chunk states its own size in hex, and a zero-size chunk ends the body:

```
POST /echo HTTP/1.1
Transfer-Encoding: chunked

5\r\nhello\r\n6\r\n world\r\n0\r\n\r\n
```

Anything after that zero and before the blank line is the trailer section, which
this server reads and **drops**. Trailers arrive after the head has already been
validated, so merging one in would let a field arrive somewhere nothing checks it.

## Bodies, and what happens to the connection

Every row below is one HTTP/1.1 connection carrying a `POST /echo` with the
framing named, followed by a `GET /hello/world`. What matters is the second
column: whether that follow-up was answered on the *same* connection. On HTTP/1.0
every row would close, for the reason above.

| Framing on the POST                 | Responses      | `connection: close` |
| ----------------------------------- | -------------- | ------------------- |
| a 3-byte body                       | `200`, `200`   | no                  |
| `Content-Length: 0`                 | `200`, `200`   | no                  |
| no `Content-Length` at all          | `200`, `200`   | no                  |
| a `GET` carrying a body             | `200`, `200`   | no                  |
| a 2 MB body (over the 1 MiB limit)  | `413`, `200`   | no                  |
| `Content-Length: 9000000`           | `413`          | yes                 |
| `Transfer-Encoding: chunked`        | `200`, `200`   | no                  |
| `Transfer-Encoding: gzip, chunked`  | `501`          | yes                 |
| `Transfer-Encoding: chunked, gzip`  | `400`          | yes                 |
| chunked **and** a `Content-Length`  | `400`          | yes                 |
| `Content-Length: abc`               | `400`          | yes                 |
| `Content-Length: 3` given twice     | `400`          | yes                 |

The first row is the one worth staring at. Before bodies were read, those three
bytes stayed in the buffer and the next request line parsed as `abcGET`, so an
ordinary `GET` came back `501 Not Implemented` and the connection died. Framing
does not consult the method, which is why row four behaves the same way.

The chunked rows are the §6.1 and §6.3 decision in miniature. `chunked` last
means the body's end is findable, so it is read like any other body and the
connection survives. A coding *under* chunked leaves the end findable but the
content undecodable, which is a `501`. Chunked anywhere but last, or a coding
list with no chunked at all, leaves nowhere to stop reading, so there is nothing
to resume from. And chunked alongside a `Content-Length` is refused before either
is used for framing: two recipients preferring different fields is precisely how
a request gets smuggled past one of them.

Rows five and six are the same status with opposite consequences. A body over
the limit is refused on its declared `Content-Length` before a byte of it is
buffered — so the length is known, the reader steps over it, and the connection
survives. Past the drain ceiling there is no length worth reading past, so that
refusal closes. See [Refusing a request is not the same as losing the
stream](../docs/design/07-bodies-and-persistence.md#refusing-a-request-is-not-the-same-as-losing-the-stream).

## Reproducing the table

`curl` will not show you row five. It abandons an upload the moment an early
response arrives, so it cannot reuse a connection it stopped mid-body on — the
follow-up opens a new one, which is the client's choice and not the server's. A
socket that finishes what it started sees the reuse:

```python
import re, socket

def run(label, declared, body_len, follow=True):
    s = socket.create_connection(("localhost", 8080)); s.settimeout(5)
    s.sendall(f"POST /echo HTTP/1.1\r\nHost: localhost\r\n"
              f"Content-Length: {declared}\r\n\r\n".encode())
    sent = 0
    while sent < body_len:
        n = min(65536, body_len - sent)
        s.sendall(b"b" * n); sent += n
    if follow:
        s.sendall(b"GET /hello/world HTTP/1.1\r\nHost: localhost\r\n\r\n")
    s.shutdown(socket.SHUT_WR)

    out = b""
    while True:
        chunk = s.recv(65536)
        if not chunk:
            break
        out += chunk
    s.close()

    text = out.decode(errors="replace")
    # Not a line split: a response body need not end in CRLF, so the next status
    # line can share a "line" with the previous body.
    print(label, re.findall(r"HTTP/1\.1 (\d{3} [^\r\n]+)", text),
          "close" if "connection: close" in text else "open")

run("3-byte body ", 3, 3)
run("2MB body    ", 2000000, 2000000)
run("9MB declared", 9000000, 0, follow=False)
```

## Serving more than one client

Each connection gets its own thread, so a client holding a persistent connection
open no longer keeps anyone else waiting:

```python
import socket, subprocess

# One client asks, is answered, and simply holds on.
idle = socket.create_connection(("localhost", 8080))
idle.sendall(b"GET /hello HTTP/1.1\r\nHost: x\r\n\r\n")
print(idle.recv(200).split(b"\r\n")[0].decode())

# A second client, while the first is still connected.
print(subprocess.run(["curl", "-s", "-m", "3", "-o", "/dev/null", "-w", "%{http_code}",
                      "http://localhost:8080/hello"], capture_output=True, text=True).stdout)
```

Both answer `200`. Served one at a time, the second would have waited for as long
as the first cared to hold on, which is not long enough for a browser to be the
first.

A connection that goes quiet is not held for ever: every read carries a deadline,
and a connection that says nothing until the idle deadline passes is closed. The
example sets that to ten seconds. If half a request had arrived, the client is
told why:

```console
$ { printf 'GET /hel'; sleep 15; } | nc localhost 8080
HTTP/1.1 408 Request Timeout
content-type: text/plain; charset=utf-8
connection: close
content-length: 20

408 Request Timeout
```

The `sleep` is what makes this work. Piping `printf` straight into `nc` closes the
sending side as soon as the bytes are out, and a client that closed is one that
finished rather than one that stalled, so the server hangs up without a word.

A connection that had finished its last request and simply went idle is closed
without a reply, because a client that asked for nothing is owed nothing.

Feeding a request slowly does not help either. The deadline for finishing a
request runs from its first byte, not from its last, so a client sending one byte
at a time is cut off on the same schedule as one sending nothing:

```console
$ # a byte every second, against a ten second budget
$ python3 -c "
import socket, time
s = socket.create_connection(('localhost', 8080))
try:
    for byte in b'GET /hello HTTP/1.1':
        s.sendall(bytes([byte])); time.sleep(1)
except BrokenPipeError:
    pass
print(s.recv(100).split(b'\r\n')[0].decode())"
HTTP/1.1 408 Request Timeout
```

The drip has to survive a broken pipe to print anything, and that is the
deadline firing rather than an error: the server answered and hung up while the
client was still on its twelfth byte. The `408` it sent is there to read.

A client that stops *reading* is bounded by the same deadline applied to sending,
so a response too large to sit in the socket buffers cannot hold a thread while
nobody collects it. Neither can a client that reads just enough to keep the sender
going: the deadline covers the whole response rather than each send, so taking it
a sip at a time buys no more time than refusing it outright. A response small
enough to fit is already gone by the time the client ignores it, and holds
nothing.

How many threads there are is bounded too. A fixed pool of workers takes accepted
connections off a queue, so the count is chosen rather than discovered, and it
does not move when clients arrive:

```console
$ ./build/debug/bin/hello &
$ ls /proc/$!/task | wc -l
17
$ # 80 connections, each sending a partial head and then holding on
$ ls /proc/$!/task | wc -l
17
```

Seventeen is the main thread and the sixteen workers the example asks for. One
worker serves a whole
connection rather than one request, so a client holding a keep-alive connection
holds a worker with it. Past sixty four, connections wait in the queue; past the
queue, they are closed as they arrive rather than held. A connection that waited
in the queue longer than the queue deadline is dropped when a worker finally
reaches it, on the grounds that the client has very likely gone.

Both bounds are arguments to `run`, and the numbers above are the ones the
example passes. See *Choosing the bounds* below.

Reaching that limit is survivable but not comfortable. Accepting fails with
`EMFILE`, the loop waits and asks again, and the server serves normally the moment
a connection finishes and gives a descriptor back:

```console
$ ( ulimit -n 64; ./build/debug/bin/hello ) &     # a deliberately small budget
$ # 80 connections, each sending a partial head and then nothing
before the flood        200
while exhausted         timed out
after they hang up      200
```

The middle row is the honest part: the server is alive throughout, and unable to
answer anyone while every descriptor is held by a client that will not let go.
The deadlines are what get those descriptors back without waiting for the client
to relent. The pool bounds threads, not descriptors: a connection sitting in the
queue still holds one.

## Choosing the bounds

`run` takes the pool limits and the deadlines after the port. The example sets
both rather than leaving them at their defaults, which is what makes the queue
reachable by hand:

```cpp
// Sixteen connections served at once, sixty four more waiting for a worker, and
// a connection that waited two seconds dropped rather than served.
constexpr carafe::PoolLimits limits{16, 64, std::chrono::seconds(2)};

// A connection silent for ten seconds is closed, and a request has ten seconds
// to arrive and to be answered.
constexpr carafe::Deadlines deadlines{std::chrono::seconds(10),
                                      std::chrono::seconds(10)};

// run() has no success to return: it serves until something stops it.
const carafe::RunError failure = app.run(port, limits, deadlines);
std::cerr << "carafe stopped: " << carafe::describe(failure) << '\n';
```

`RunError` says which failure it was, because there are now three that a `bool`
would have run together: `InvalidLimits`, `InvalidDeadlines`, `BindFailed` and
`AcceptFailed`. Reporting a rejected deadline as a port that would not bind
sends the reader to the wrong problem. There is no `None`: `run` returns nothing but
failures, and `describe` turns one into a line worth printing.

Both arguments default, so `app.run(8080)` still compiles. The defaults are
sixty four workers, a queue of five hundred and twelve, and thirty seconds for
each deadline.

Zero is refused rather than honoured, and `run` returns `false` before
binding anything. No workers means nothing ever takes from the queue. No queue
room means every arrival is closed even while every worker sits idle, because a
worker is only ever handed work through the queue. And a zero deadline reaches
`setsockopt` as *no* deadline at all, which is the opposite of how it reads. Ask
for no limit with a large value instead.

With sixteen and sixty four, the far end of the queue is one command away.
Sixteen connections take the workers, sixty four more fill the queue, and the
next one is closed as it arrives:

```console
$ python3 -c "
import socket
held = [socket.create_connection(('localhost', 8080)) for _ in range(80)]
for s in held:
    s.sendall(b'GET /hel')
extra = socket.create_connection(('localhost', 8080)); extra.settimeout(5)
print(repr(extra.recv(200)))"
b''
```

The empty read is the server having closed it without a word. Sending first
gets a reset instead of an end, because closing a socket that still holds unread
bytes is a reset rather than a clean finish. Either way the client learns at
once, which is the point: the accept loop cannot afford to wait on a `503` it
would have to write itself.
