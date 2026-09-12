# Build and tooling

The language standard, where headers live, how flags reach a target, and what
the gates check. Part of the [design notes](README.md).

## C++17 is a choice, not a constraint

The toolchain in use is far newer than the floor in the requirements table, so
C++20 is available and unused. It would bring `std::span` for buffer handling
and `starts_with`/`ends_with` for parsing; C++23 would bring `std::expected`.
Staying on 17 is deliberate — writing that plumbing by hand is most of the point
of the exercise. Revisit only if a real problem needs a feature 17 lacks, not
for convenience.

## Headers are listed, not declared as a file set

`CARAFE_HEADERS` is a plain variable passed to `add_library()`, which affects
only how IDEs display the project — compilation finds headers through include
paths. CMake 3.23 added `FILE_SET HEADERS`, which declares them as a real target
property and lets `install(TARGETS)` handle header installation directly. It is
available here and deliberately unused: its payoff is in install and export
rules, and this project has none. The day `find_package(carafe)` needs to work
from another project, migrate to a file set and add the export rules as one
change — they solve the same problem, and splitting them leaves the build half
converted.

## A header's location is the statement of support

The build lives at the top level — `CMakeLists.txt`, `CMakePresets.json`, the
`Makefile`, and the modules under `cmake/`. Source is split across `include/`,
`src/`, `tests/`, and `examples/`, prose lives in `docs/`, and each preset
builds into its own git-ignored `build/<preset>/`.

The boundary that matters is public versus internal. `include/carafe/` is the
library's API surface: anything there is something a user of the framework may
rely on, and it is included as `#include <carafe/foo.hpp>`. Everything else
stays in `src/`, beside the code it serves, and is included with quotes —
`#include "http/request_parser.hpp"`. That spelling works because `src/` is on
the include path `PRIVATE`ly, so it resolves inside the library and nowhere
else. Tests cross the boundary on purpose by putting `src/` on their own include
path; nothing else does, and a header's location is the statement of whether it
is supported.

The alternative is a header that says so in a comment, or a `detail` namespace
inside a public header. Both leave the file installable and includable, so the
statement is advice. Keeping internal headers out of `include/` makes it
structural: a consumer cannot reach `router.hpp` to depend on it by mistake,
which is the same reason `App` holds its `Router` behind a forward declaration.

Both trees group by subsystem — `http/`, `net/` and `server/` today — and
`tests/` mirrors that shape with one `*_test.cpp` per source file. Generated
headers, currently only `version.hpp` expanded from its `.in` template, land in
`build/<preset>/generated/carafe/` and are included exactly like hand-written
public ones; whoever includes them cannot tell the difference.

## Warnings are private, sanitizer flags are public

Two flag conventions are worth knowing before adding a target. Warnings are
`PRIVATE`, so they never leak into a consumer's build and every target must ask
for them by calling `carafe_target_warnings()`. Sanitizer and coverage flags are
`PUBLIC` on the `carafe` target, because ASan has to instrument every
translation unit that lands in one binary — so anything linking `carafe::carafe`
inherits them automatically and must *not* apply them again.

The asymmetry is the point. A warning is an opinion about carafe's own source
and a consumer is entitled to a different one; an instrumented build is a
property of the whole binary, and a half-instrumented one reports faults that
are artefacts of the mixture rather than bugs in either half.

## The gate could not see the headers it guarded

An audit of `[[nodiscard]]`, `noexcept`, `constexpr`, `explicit` and const
member functions found no correctness bug and one defect, but the more
important result was a blind spot in the tidy gate itself.

clang-tidy diagnoses a header only when the header's path matches
`HeaderFilterRegex`, whichever file includes it. The filter matched
`include/carafe/` and the generated version header and nothing else, so
nothing declared in `src/` or `tests/` headers was ever checked. That is where
the defect lived: `ConnectionPool`'s constructor could be called with one
argument, so a router pointer would quietly build a running pool. The proof
was mechanical. With `explicit` removed, a run shaped exactly like the gate
passed; with the filter widened, the same run fails on that line. Widening it
also surfaced four findings that had been sitting there unseen: unnamed
parameters on the socket's move operations, a redundant initialiser on a
`time_point`, and a pool that declared its destructor and deleted copying
without saying anything about moving. All four are fixed, and the pool now
deletes its moves with the reason stated, since every worker holds `this`.

The new pattern admits `src/<dir>/<file>.hpp` and test headers by shape rather
than by naming the repository directory, because a checkout need not be called
carafe. A measured run over the whole gate found diagnostics only in project
headers, so the fetched GoogleTest sources stay out.

`google-explicit-constructor` is enabled on its own rather than as part of the
google group, whose other checks restate style this project already decides.
Its measured cost across the library, the tests and the example was the one
real finding.

Four rules have no check that can hold them, so they are recorded here.

`[[nodiscard]]` goes on every function whose result the caller has to act on,
free or member. `modernize-use-nodiscard` only considers member functions, so
file-local helpers are a matter of review.

`noexcept` goes wherever nothing can throw: no allocation, and no `substr`
from a position not already proven in range. `bugprone-exception-escape` is
weaker than its name. Probed directly, it reported a literal `throw` and
missed both an allocation and an out-of-range `substr`, so a `noexcept` is
justified by reading the body.

`constexpr` goes in headers, where it also implies `inline` and lets any
includer evaluate a helper at compile time, and on named constants. It stays
off file-local functions in source files. Seven of them carried it with no
rule and no effect: none was used in a constant expression. An earlier
formulation, `constexpr` only on classifiers taking a character or an enum,
was rejected because headers already used it on functions that walk strings
and three eligible source helpers never had it.

A member function is `const` by logical constness, not bitwise. The socket's
read, write and timeout setter leave its fields untouched but change the
socket they name, so they are non-const and say why where the const check
would otherwise ask.
