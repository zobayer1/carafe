#pragma once

#include <string>
#include <string_view>

namespace carafe::http {

// The path as routing should see it, so a pattern and a request target cut the same way. Escapes of unreserved
// characters are resolved and every other escape keeps its spelling with the hex digits uppercased; "." segments are
// dropped and ".." segments pop the one before them. RFC 3986 §6.2.2 and §5.2.4.
//
// A path, not a target: cut the query first. What it leaves alone is as much of the contract as what it changes. An
// escaped separator stays escaped, so no client can spell a boundary the split will not see. An empty segment is
// kept, so "//a" is not "/a". A trailing slash is kept, and a trailing "." or ".." leaves one. ".." at the root pops
// nothing rather than failing. A malformed escape is copied as it stands, for the reason find() states.
[[nodiscard]] std::string normalize_path(std::string_view path);

}  // namespace carafe::http
