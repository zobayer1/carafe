# carafe

[![ci](https://github.com/zobayer1/carafe/actions/workflows/ci.yml/badge.svg)](https://github.com/zobayer1/carafe/actions/workflows/ci.yml)
[![C++17](https://img.shields.io/badge/C%2B%2B-17-blue.svg)](https://en.cppreference.com/w/cpp/17)
[![platform](https://img.shields.io/badge/platform-Linux-lightgrey.svg)](#requirements)
[![license](https://img.shields.io/badge/license-MIT-green.svg)](LICENSE)

A small HTTP microframework for C++17, in the spirit of Flask — routes, handlers,
requests, responses, and not much else.

This is a learning project, built from the socket up rather than on top of an
existing HTTP library. It is not production software.

**Status:** it routes. Static paths and single-segment path parameters, matched
by method, with a 404 for an unregistered path and a 405 naming the methods that
would have worked. Malformed heads get their proper statuses — 400, 413, 414,
431, 501, 505 — and a request refused for its size is answered without ending the
connection. A body declared by `Content-Length` reaches the handler whole.
Connections are kept alive and handled one at a time.

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
app.run(8080);
```

`make run` serves the whole example on port 8080. See
[docs/examples.md](docs/examples.md) for every route it registers, and for what
each kind of body does to the connection.

## Goals

- Understand HTTP/1.1 by implementing it rather than reading about it.
- Practice modern C++: RAII, move semantics, templates, the standard library.
- Practice the surrounding craft: CMake, unit tests, sanitizers, static analysis.

<!-- Fill in as the design settles. -->

## Requirements

| Tool         | Version | Notes                                     |
| ------------ | ------- | ----------------------------------------- |
| Linux        | ≥ 2.6.28 | `accept4` sets the floor; `SOCK_CLOEXEC` alone needs only 2.6.27 |
| CMake        | ≥ 3.25  | presets v6 sets the floor; `FIND_PACKAGE_ARGS` needs only 3.24 |
| C++ compiler | C++17   | GCC and Clang; CI builds both on Ubuntu 24.04 |
| GoogleTest   | 1.17.0  | fetched automatically, or found on-system |

Linux only, and by decision rather than omission. `Listener` calls `accept4`
with `SOCK_CLOEXEC` so an accepted descriptor is close-on-exec from the moment
it exists — neither macOS nor Windows has either, and neither has a race-free
substitute. The syscall arrived in Linux 2.6.28 and glibc wrapped it in 2.10, so
on a glibc system that pair is the real floor. Every live distribution clears it
by well over a decade, which makes it easy to leave unstated and no less a
requirement. Portability means a socket layer rather than a patch, so it is a
milestone and not a footnote. No older compiler floor is claimed here because
none is tested; CI builds what Ubuntu 24.04 ships.

Optional: `ccache` (used automatically if present), `clang-format`, `clang-tidy`,
`gcovr`. On Fedora, the sanitizer build additionally needs
`sudo dnf install libasan libubsan`.

## Quick start

```sh
make            # list the available targets
make test       # configure, build, and run the test suite
make run        # serve on http://localhost:8080 until interrupted
```

The Makefile is only a shortcut layer — it holds no build settings of its own
and every target shells out to a preset, so `CMakePresets.json` stays the single
source of truth. The same thing without it:

```sh
cmake --preset debug
cmake --build --preset debug
ctest --preset debug
```

There are four presets — `debug`, `release`, `asan`, `coverage` — and each
builds into its own `build/<preset>/` tree, so they never clobber each other.
`make` uses `debug` unless told otherwise:

```sh
make PRESET=release test
```

## Make targets

| Target         | What it does                                            |
| -------------- | ------------------------------------------------------- |
| `help`         | List targets (the default goal)                         |
| `configure`    | Configure the current preset                            |
| `build`        | Build the current preset, configuring first if needed   |
| `test`         | Build, then run the suite through CTest                 |
| `run`          | Serve on port 8080 until interrupted (Ctrl-C)           |
| `debug`        | Build + test the `debug` preset                         |
| `release`      | Build + test the `release` preset                       |
| `asan`         | Build + test with AddressSanitizer + UBSan              |
| `coverage`     | Build + test with gcov, then summarize coverage         |
| `format`       | Reformat all sources in place with clang-format         |
| `format-check` | Fail if any source is not formatted                     |
| `tidy`         | Run clang-tidy over the project sources                 |
| `compdb`       | Symlink `compile_commands.json` into the project root   |
| `clean`        | Remove build artifacts for the current preset           |
| `distclean`    | Remove every build directory                            |

`PRESET` (default `debug`) and `JOBS` can be overridden per invocation, e.g.
`make PRESET=asan test`.

## CMake options

| Option                      | Default        | Effect                       |
| --------------------------- | -------------- | ---------------------------- |
| `CARAFE_BUILD_TESTS`        | ON (top-level) | Build the unit tests         |
| `CARAFE_BUILD_EXAMPLES`     | ON (top-level) | Build the example programs   |
| `CARAFE_WARNINGS_AS_ERRORS` | OFF            | Add `-Werror` / `/WX`        |
| `CARAFE_ENABLE_SANITIZERS`  | OFF            | Instrument with ASan + UBSan |
| `CARAFE_ENABLE_COVERAGE`    | OFF            | Instrument with gcov         |

## Roadmap

<!-- Check these off as they land. -->

- [x] Descriptor ownership: a move-only `Socket` that closes exactly once
- [x] TCP listener: socket, bind and listen, on a port the kernel may choose
- [x] Accepting connections: `accept()`, one `Socket` per client
- [x] Connections: socket bytes assembled into successive requests
- [x] HTTP/1.1 request line parsing
- [x] Line splitting: scan position across reads, length cap, CRLF policy
- [x] Header field parsing: token names, OWS, obs-fold and CTL rejection
- [x] Header block assembly: duplicates, Host, total-head limit
- [x] Request body via Content-Length
- [x] Draining a refused body so the connection outlives the refusal
- [x] Response building and serialization
- [x] Routing: static paths matched by method, with 404 and 405 + `Allow`
- [x] Handler registration API
- [x] Routing: path parameters, one segment each
- [x] Routing: percent-decoded captures
- [ ] Routing: typed and multi-segment parameters
- [ ] Routing: normalising a pattern and a request path against each other
- [x] Concurrency: a thread per connection, detached
- [x] Concurrency: a receive deadline, so an idle connection stops holding a thread
- [x] Concurrency: a deadline on a whole request, and on writing a response
- [ ] Concurrency: a thread pool, to bound how many connections may be held at once
- [ ] Middleware
- [x] Keep-alive: the `Connection` field and the version's own default
- [x] Chunked transfer encoding: chunk framing, extensions, and a dropped trailer section
- [ ] Static file serving
- [ ] Portability: a socket layer that is not Linux-only

## Design notes

Every non-obvious decision, and what was rejected to reach it, lives in
[docs/design-notes.md](docs/design-notes.md) — including how the tree is laid
out, where the public/internal boundary runs, and which flags are `PRIVATE`.

## License

MIT — see [LICENSE](LICENSE). Copyright (c) 2026 Zobayer Hasan.
