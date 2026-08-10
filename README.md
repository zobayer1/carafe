# carafe

A small HTTP microframework for C++17, in the spirit of Flask — routes, handlers,
requests, responses, and not much else.

This is a learning project, built from the socket up rather than on top of an
existing HTTP library. It is not production software.

**Status:** line splitting, request line and header field parsing, with their
tests. No server yet.

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
make run        # build and run the hello example
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
| `run`          | Build and run the `hello` example                       |
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
`src/`, `tests/`, and `examples/`, and each preset builds into its own
git-ignored `build/<preset>/`.

The boundary that matters is public versus internal. `include/carafe/` is the
library's API surface: anything there is something a user of the framework may
rely on, and it is included as `#include <carafe/foo.hpp>`. Everything else
stays in `src/`, beside the code it serves, and is included with quotes —
`#include "http/request_parser.hpp"`. That spelling works because `src/` is on
the include path `PRIVATE`ly, so it resolves inside the library and nowhere
else. Tests cross the boundary on purpose by putting `src/` on their own include
path; nothing else does, and a header's location is the statement of whether it
is supported.

Both trees group by subsystem — `http/` today, `net/` and `routing/` later — and
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

- [ ] TCP listener: socket, bind, listen, accept
- [x] HTTP/1.1 request line parsing
- [x] Line splitting: scan position across reads, length cap, CRLF policy
- [x] Header field parsing: token names, OWS, obs-fold and CTL rejection
- [ ] Header block assembly: duplicates, Host, total-head limit
- [ ] Request body via Content-Length
- [ ] Response building and serialization
- [ ] Routing: static paths, then path parameters
- [ ] Handler registration API
- [ ] Concurrency: thread-per-connection, then a thread pool
- [ ] Middleware
- [ ] Keep-alive, chunked transfer encoding
- [ ] Static file serving

## Design notes

<!-- Record decisions here as they are made, with the reasoning behind them. -->

### C++17 is a choice, not a constraint

The toolchain in use is far newer than the floor in the requirements table, so
C++20 is available and unused. It would bring `std::span` for buffer handling
and `starts_with`/`ends_with` for parsing; C++23 would bring `std::expected`.
Staying on 17 is deliberate — writing that plumbing by hand is most of the point
of the exercise. Revisit only if a real problem needs a feature 17 lacks, not
for convenience.

### A parsed request owns its strings

`Request` holds owning `std::string` members. `std::string_view` is used heavily
*inside* the parser for zero-copy slicing and comparison, but owned strings are
materialized at the boundary when a field is committed.

This is not the zero-copy design, and it is chosen with that in mind. Views into
a growing connection buffer cannot be stored incrementally: the next `append`
may reallocate and dangle every view already kept. The two ways out are to
re-parse after each growth — which makes scanning quadratic in the number of
chunks, and the attacker picks that number — or to store realloc-stable
`{offset, length}` pairs and resolve them to views at the end. The second is
correct, and it makes `Request` a bag of integers that is meaningless without
the buffer it indexes into, with every accessor needing that buffer passed back
in. That API burden infects handlers, the router, and anything that wants to
keep a header past the life of the connection.

Owning is not a complexity regression — it is O(n), one extra pass over bytes
already scanned. The cost is allocation count, and small-string optimization
absorbs most of it, since header names and methods are nearly all under the
15-character inline threshold. Revisit once real socket buffers exist *and* a
benchmark says the copying matters; either alone is not enough.

Keeping a scan position across reads is a separate matter and not deferred with
this. Resuming the `\r\n\r\n` search from `scanned - 3` rather than from zero is
a hostile-input requirement, independent of who owns the bytes.

### Parse failures carry a reason

Parsing returns a small struct — an error enum plus the parsed value, with an
`explicit operator bool` — rather than throwing or returning a bare
`std::optional`. HTTP needs to know *which* failure occurred: a malformed
request line is 400, an unknown method is 501, an unsupported version is 505,
and `optional` discards exactly that. Exceptions were the other candidate and
were rejected because this is a hot path fed by hostile input, where malformed
requests are ordinary traffic rather than an exceptional condition.

The error enum names semantic failures, not status codes; mapping them to
responses belongs to the HTTP layer, so the parser stays usable by something
that serves no responses at all. The result struct is a hand-rolled
`std::expected` — it should be generalized into a `Result<T>` template once
there is a second use site to shape it, and not before.

The rule is conditional on there being more than one failure to tell apart.
`parse_header_field` has exactly one — a malformed field is 400 and nothing
else — so it returns a bare `std::optional` and inventing an error enum for it
would add a state no code path can produce. `LineReader` went the other way for
the opposite reason: it needed a *third* state, since "no line yet" is not a
failure. Each layer's return type is counted from its own states rather than
copied from its neighbour.

### The line splitter owns the terminator

`parse_request_line` receives the bytes of a line with CRLF already removed, and
never sees or strips a terminator itself. The alternative — tolerating a trailing
`\r` in the parser — means two components both know about terminators, and a bare
`\n` line ending behaves differently from `\r\n` for reasons no single file explains.

Stripping the terminator does not make the line free of CR and LF: a `\r` not
followed by `\n` survives a two-byte split and lands in the target, which is a
smuggling vector when a proxy and this server disagree about where the line ends.
The parser therefore rejects every control byte in the line, which also costs it
nothing to reject NUL and tab. Length capping stays with the splitter, since by
the time the parser is called the bytes are already buffered and the memory is
already spent.

### Field names are normalized, field values are not

Header field names are case-insensitive, so `parse_header_field` lowercases them
once at parse time rather than leaving every lookup to compare case-insensitively.
It also makes the wire format and the in-memory format agree with HTTP/2, which
mandates lowercase names outright. Values are left exactly as received: casing
carries meaning in base64 credentials, entity tags, and URLs, and a parser that
folds it is corrupting data rather than normalizing it.

Both the name and the value are validated against allowlists taken from the
grammar — `tchar` for names, `field-vchar` plus SP and HTAB for values — rather
than against a list of forbidden bytes. A denylist over an open set is a hole by
construction: an earlier version named CR, LF, and DEL, and let NUL through into
a value, which is a truncation vector the moment that string meets a C API. An
allowlist fails closed on the byte nobody thought of.

### Headers are listed, not declared as a file set

`CARAFE_HEADERS` is a plain variable passed to `add_library()`, which affects
only how IDEs display the project — compilation finds headers through include
paths. CMake 3.23 added `FILE_SET HEADERS`, which declares them as a real target
property and lets `install(TARGETS)` handle header installation directly. It is
available here and deliberately unused: its payoff is in install and export
rules, and this project has none. The day `find_package(carafe)` needs to work
from another project, migrate to a file set and add the export rules as one
change — they solve the same problem, and splitting them leaves the build half
converted.

## License

MIT — see [LICENSE](LICENSE). Copyright (c) 2026 Zobayer Hasan.
