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

## Bodies, and what happens to the connection

Every row below is one connection carrying a `POST /echo` with the framing named,
followed by a `GET /hello/world`. What matters is the second column: whether that
follow-up was answered on the *same* connection.

| Framing on the POST                 | Responses      | `connection: close` |
| ----------------------------------- | -------------- | ------------------- |
| a 3-byte body                       | `200`, `200`   | no                  |
| `Content-Length: 0`                 | `200`, `200`   | no                  |
| no `Content-Length` at all          | `200`, `200`   | no                  |
| a `GET` carrying a body             | `200`, `200`   | no                  |
| a 2 MB body (over the 1 MiB limit)  | `413`, `200`   | no                  |
| `Content-Length: 9000000`           | `413`          | yes                 |
| `Transfer-Encoding: chunked`        | `501`          | yes                 |
| `Content-Length: abc`               | `400`          | yes                 |
| `Content-Length: 3` given twice     | `400`          | yes                 |

The first row is the one worth staring at. Before bodies were read, those three
bytes stayed in the buffer and the next request line parsed as `abcGET`, so an
ordinary `GET` came back `501 Not Implemented` and the connection died. Framing
does not consult the method, which is why row four behaves the same way.

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
