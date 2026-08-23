# carafe

A small HTTP microframework for C++17, in the spirit of Flask — routes, handlers,
requests, responses, and not much else.

This is a learning project, built from the socket up rather than on top of an
existing HTTP library. It is not production software.

**Status:** it routes. `make run` starts a server with two registered routes;
anything else gets a 404, and a known path under the wrong method a 405 naming
the methods that would have worked.
Malformed heads still get their proper statuses — 400, 414, 431, 501, 505.
Connections are kept alive and handled one at a time. Paths must match exactly,
and there is no request body yet.

```cpp
carafe::App app;
app.get("/hello", [](const carafe::http::Request&) {
    return carafe::http::text_response(200, "hello\n");
});
app.run(8080);
```

## Goals

- Understand HTTP/1.1 by implementing it rather than reading about it.
- Practice modern C++: RAII, move semantics, templates, the standard library.
- Practice the surrounding craft: CMake, unit tests, sanitizers, static analysis.

<!-- Fill in as the design settles. -->

## Requirements

| Tool         | Version | Notes                                     |
| ------------ | ------- | ----------------------------------------- |
| CMake        | ≥ 3.25  | 3.24 for `FIND_PACKAGE_ARGS`, 3.25 for presets v6 |
| C++ compiler | C++17   | GCC 9+, Clang 10+, MSVC 2019+             |
| GoogleTest   | 1.17.0  | fetched automatically, or found on-system |

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

## Layout

The build lives at the top level — `CMakeLists.txt`, `CMakePresets.json`, the
`Makefile`, and the modules under `cmake/`. Source is split across `include/`,
`src/`, `tests/`, and `examples/`, prose lives in `docs/`, and each preset builds
into its own git-ignored `build/<preset>/`.

The boundary that matters is public versus internal. `include/carafe/` is the
library's API surface: anything there is something a user of the framework may
rely on, and it is included as `#include <carafe/foo.hpp>`. Everything else
stays in `src/`, beside the code it serves, and is included with quotes —
`#include "http/request_parser.hpp"`. That spelling works because `src/` is on
the include path `PRIVATE`ly, so it resolves inside the library and nowhere
else. Tests cross the boundary on purpose by putting `src/` on their own include
path; nothing else does, and a header's location is the statement of whether it
is supported.

Both trees group by subsystem — `http/`, `net/` and `server/` today — and
`tests/` mirrors that shape with one `*_test.cpp` per source file. Generated
headers, currently only `version.hpp` expanded from its `.in` template, land in
`build/<preset>/generated/carafe/` and are included exactly like hand-written
public ones; whoever includes them cannot tell the difference.

Two flag conventions are worth knowing before adding targets. Warnings are
`PRIVATE`, so they never leak into a consumer's build and every target must ask
for them by calling `carafe_target_warnings()`. Sanitizer and coverage flags are
`PUBLIC` on the `carafe` target, because ASan has to instrument every
translation unit that lands in one binary — so anything linking `carafe::carafe`
inherits them automatically and must *not* apply them again.

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
- [ ] Request body via Content-Length
- [x] Response building and serialization
- [x] Routing: static paths matched by method, with 404 and 405 + `Allow`
- [x] Handler registration API
- [ ] Routing: path parameters
- [ ] Concurrency: thread-per-connection, then a thread pool
- [ ] Middleware
- [ ] Keep-alive, chunked transfer encoding
- [ ] Static file serving

## Design notes

Every non-obvious decision, and what was rejected to reach it, lives in
[docs/design-notes.md](docs/design-notes.md).

## License

MIT — see [LICENSE](LICENSE). Copyright (c) 2026 Zobayer Hasan.
