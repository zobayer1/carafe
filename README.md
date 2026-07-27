# carafe

A small HTTP microframework for C++17, in the spirit of Flask — routes, handlers,
requests, responses, and not much else.

This is a learning project, built from the socket up rather than on top of an
existing HTTP library. It is not production software.

**Status:** scaffolding only. No server yet.

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

```
.
├── CMakeLists.txt        top-level build: options, global settings, subdirectories
├── CMakePresets.json     named configure/build/test presets — the build's source of truth
├── Makefile              convenience shortcuts; delegates to the presets
├── cmake/
│   ├── CompilerWarnings.cmake   the project's warning set
│   └── Sanitizers.cmake         ASan/UBSan and gcov instrumentation
├── include/carafe/       public headers — this is the library's API surface
│   └── version.hpp.in    template; CMake expands it into build/<preset>/generated/
├── src/                  implementation, plus the library target
├── tests/                GoogleTest suite; also where GoogleTest is fetched
├── examples/             small programs that use the library
└── build/<preset>/       one build tree per preset (git-ignored)
```

Public headers go in `include/carafe/` and are included as
`#include <carafe/foo.hpp>`. Anything not meant for users stays in `src/`.

Two flag conventions are worth knowing before adding targets. Warnings are
`PRIVATE`, so they never leak into a consumer's build and every target must ask
for them by calling `carafe_target_warnings()`. Sanitizer and coverage flags are
`PUBLIC` on the `carafe` target, because ASan has to instrument every
translation unit that lands in one binary — so anything linking `carafe::carafe`
inherits them automatically and must *not* apply them again.

## Roadmap

<!-- Check these off as they land. -->

- [ ] TCP listener: socket, bind, listen, accept
- [ ] HTTP/1.1 request parsing (request line, headers, body)
- [ ] Response building and serialization
- [ ] Routing: static paths, then path parameters
- [ ] Handler registration API
- [ ] Concurrency: thread-per-connection, then a thread pool
- [ ] Middleware
- [ ] Keep-alive, chunked transfer encoding
- [ ] Static file serving

## Design notes

<!-- Record decisions here as they are made, with the reasoning behind them. -->

## License

MIT — see [LICENSE](LICENSE). Copyright (c) 2026 Zobayer Hasan.
