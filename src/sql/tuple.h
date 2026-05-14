#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// SQL column types we know how to serialize.
//   Int32 / Int64 / Bool — fixed width.
//   Text                  — variable width, length-prefixed (uint32 + bytes).
enum class Type { Int32, Int64, Bool, Text };

struct Column {
    std::string name;
    Type type;
    bool nullable;
};

// A row's schema: an ordered list of columns. Pure value type; no I/O.
struct Schema {
    std::vector<Column> columns;

    // Sentinel returned by indexOf when the name isn't present. Mirrors
    // std::string::npos's "size_t with all bits set" convention.
    static constexpr size_t kNotFound = static_cast<size_t>(-1);

    // Find a column by name. Returns kNotFound if absent.
    size_t indexOf(const std::string& name) const;

    // Bytes a tuple of this schema occupies on disk *if every column is
    // fixed-width*. Throws when the schema contains a Text column, since
    // the encoded size then depends on the actual values. Use the size of
    // TupleCodec::encode(...) instead for variable-length schemas.
    size_t tupleSize() const;
};

// A runtime value. The union is only safe to read for the discriminator's
// type when is_null is false; otherwise the payload is unset.
struct Value {
    Type type;
    bool is_null;
    union {
        int32_t i32;
        int64_t i64;
        bool    b;
    };
    std::string text;  // out-of-union for simplicity (Text reserved)

    // Convenience builders. Make tests and call-sites read cleanly.
    static Value Int32(int32_t v);
    static Value Int64(int64_t v);
    static Value Bool(bool v);
    static Value Text(std::string v);
    static Value Null(Type t);
};

bool operator==(const Value& a, const Value& b);
inline bool operator!=(const Value& a, const Value& b) { return !(a == b); }

// Stable on-disk integer encoding of a Type, used anywhere the type
// itself needs to be persisted (the catalog's __columns table is the
// current consumer). The numeric values are part of the file format —
// never reorder or reuse them.
int32_t typeToCode(Type t);
Type    typeFromCode(int32_t c);

// Map a SQL surface type keyword (e.g. "INT", "BIGINT", "TEXT") to a
// Type. Case-insensitive. Throws std::runtime_error on anything we
// don't recognise. Accepted spellings:
//   INT / INTEGER  -> Int32
//   BIGINT         -> Int64
//   BOOL / BOOLEAN -> Bool
//   TEXT           -> Text
Type typeFromName(const std::string& name);

// Render a Value to a short human-readable string. NULL shows as
// "NULL"; Bool shows as "true" / "false"; Int32/Int64 use std::to_string;
// Text is returned verbatim. Used by the demo and any future EXPLAIN /
// REPL output. Not meant to be a round-trippable SQL literal.
std::string valueToString(const Value& v);

// Stateless codec converting a row of Values to/from the byte sequence
// stored in a SlottedPage's tuple area. Layout:
//
//   [ null bitmap : ceil(N/8) bytes ][ col0 bytes ][ col1 bytes ] ...
//
// Each column's slot:
//   Int32/Int64/Bool — full fixed width regardless of null status; the
//                      bitmap is authoritative for nullness.
//   Text             — uint32_t length prefix followed by `length` bytes.
//                      Null is encoded as length 0 with no payload (and
//                      the bitmap bit set); empty-but-non-null Text is
//                      length 0 with the bitmap bit clear.
class TupleCodec {
public:
    // Throws std::runtime_error on column-count mismatch, type mismatch,
    // or null in a non-nullable column.
    static std::vector<char> encode(const Schema& s, const std::vector<Value>& vals);

    // Throws if the bytes are truncated mid-column, contain trailing bytes
    // after the schema is satisfied, or are too short for the null bitmap.
    static std::vector<Value> decode(const Schema& s, const char* bytes, size_t len);
};
