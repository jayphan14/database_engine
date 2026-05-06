#pragma once

#include <string>

// Generic string helpers. Reach for these from any layer; nothing in
// here is SQL- or storage-specific.
namespace util {

// Return an ASCII-uppercased copy of `s`. Non-ASCII bytes are passed
// through unchanged. Used wherever we need to compare a user-typed
// keyword case-insensitively (lexer keywords, type-name resolution,
// future config flags, ...).
std::string toUpper(const std::string& s);

}  // namespace util
