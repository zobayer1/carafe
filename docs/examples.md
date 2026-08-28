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

app.run(8080);
```

## Registered routes

| Method | Path             | Answers with                                  |
| ------ | ---------------- | --------------------------------------------- |
| GET    | `/`              | the version and a hint                        |
| GET    | `/hello`         | the request target it was asked for           |
| GET    | `/hello/<name>`  | a greeting using the captured segment         |
| POST   | `/echo`          | the request body, unchanged                   |
| POST   | `/size`          | the body's length in bytes                    |
| PUT    | `/store/<key>`   | the captured key and the body together        |
| PATCH  | `/store/<key>`   | the same, to show a second body verb frames alike |
| DELETE | `/store/<key>`   | the captured key alone                        |
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
curl -i -X PUT --data 'v' http://localhost:8080/store/k
curl -i -X PATCH --data 'more' http://localhost:8080/store/k
curl -i -X DELETE http://localhost:8080/store/k
curl -i -X OPTIONS http://localhost:8080/store/k
curl -i -X POST http://localhost:8080/hello          # 405, with allow:
curl -i http://localhost:8080/missing                # 404
```

A percent-escape in a captured segment is decoded, and only within that segment:

```sh
curl -i http://localhost:8080/hello/a%2Fb            # hello, a/b!
curl -i --path-as-is http://localhost:8080/hello/a%25b  # hello, a%b!
```

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
refusal closes. See *Refusing a request is not the same as losing the stream* in
[design-notes.md](design-notes.md).

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

## Serving one at a time

The example serves each connection to completion before accepting the next, so a
client holding a keep-alive connection open locks everyone else out. That is a
known gap rather than a surprise — see the concurrency milestone in the
[roadmap](../README.md#roadmap).
