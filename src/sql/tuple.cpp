#include "src/sql/tuple.h"

#include <cstring>
#include <limits>
#include <stdexcept>
#include <utility>

namespace {

// Byte width of a fixed-width column type. Text is variable-length and is
// rejected here; tupleSize() (the only caller) is documented as fixed-only.
size_t widthOf(Type t) {
    switch (t) {
        case Type::Int32: return 4;
        case Type::Int64: return 8;
        case Type::Bool:  return 1;
        case Type::Text:
            throw std::runtime_error("tuple codec: tupleSize() is undefined for "
                                     "schemas containing Text columns");
    }
    throw std::runtime_error("tuple codec: unknown type");
}

}  // namespace

size_t Schema::indexOf(const std::string& name) const {
    for (size_t i = 0; i < columns.size(); ++i) {
        if (columns[i].name == name) return i;
    }
    return kNotFound;
}

size_t Schema::tupleSize() const {
    const size_t bitmap = (columns.size() + 7) / 8;
    size_t data = 0;
    for (const auto& c : columns) data += widthOf(c.type);
    return bitmap + data;
}

Value Value::Int32(int32_t v) {
    Value r;
    r.type = Type::Int32;
    r.is_null = false;
    r.i32 = v;
    return r;
}

Value Value::Int64(int64_t v) {
    Value r;
    r.type = Type::Int64;
    r.is_null = false;
    r.i64 = v;
    return r;
}

Value Value::Bool(bool v) {
    Value r;
    r.type = Type::Bool;
    r.is_null = false;
    r.b = v;
    return r;
}

Value Value::Text(std::string v) {
    Value r;
    r.type = Type::Text;
    r.is_null = false;
    r.text = std::move(v);
    return r;
}

Value Value::Null(Type t) {
    Value r;
    r.type = t;
    r.is_null = true;
    return r;
}

int32_t typeToCode(Type t) {
    switch (t) {
        case Type::Int32: return 0;
        case Type::Int64: return 1;
        case Type::Bool:  return 2;
        case Type::Text:  return 3;
    }
    throw std::runtime_error("typeToCode: unknown Type");
}

Type typeFromCode(int32_t c) {
    switch (c) {
        case 0: return Type::Int32;
        case 1: return Type::Int64;
        case 2: return Type::Bool;
        case 3: return Type::Text;
        default:
            throw std::runtime_error("typeFromCode: invalid type code " +
                                     std::to_string(c));
    }
}

bool operator==(const Value& a, const Value& b) {
    if (a.type != b.type) return false;
    if (a.is_null != b.is_null) return false;
    if (a.is_null) return true;
    switch (a.type) {
        case Type::Int32: return a.i32 == b.i32;
        case Type::Int64: return a.i64 == b.i64;
        case Type::Bool:  return a.b == b.b;
        case Type::Text:  return a.text == b.text;
    }
    return false;
}

std::vector<char> TupleCodec::encode(const Schema& s, const std::vector<Value>& vals) {
    if (vals.size() != s.columns.size()) {
        throw std::runtime_error(
            "TupleCodec::encode: column count mismatch (got " +
            std::to_string(vals.size()) + ", expected " +
            std::to_string(s.columns.size()) + ")");
    }

    const size_t n = s.columns.size();
    const size_t bitmap_bytes = (n + 7) / 8;

    // Validate everything before allocating so encode either fully succeeds
    // or fully fails with a clear error.
    for (size_t i = 0; i < n; ++i) {
        const auto& col = s.columns[i];
        const auto& v = vals[i];
        if (v.is_null && !col.nullable) {
            throw std::runtime_error(
                "TupleCodec::encode: column '" + col.name + "' is not nullable");
        }
        if (!v.is_null && v.type != col.type) {
            throw std::runtime_error(
                "TupleCodec::encode: column '" + col.name + "' type mismatch");
        }
        if (col.type == Type::Text && !v.is_null &&
            v.text.size() > std::numeric_limits<uint32_t>::max()) {
            throw std::runtime_error(
                "TupleCodec::encode: Text value exceeds 4 GB length limit");
        }
    }

    // Compute total size once, so we allocate exactly.
    size_t total = bitmap_bytes;
    for (size_t i = 0; i < n; ++i) {
        const auto& col = s.columns[i];
        const auto& v = vals[i];
        switch (col.type) {
            case Type::Int32: total += 4; break;
            case Type::Int64: total += 8; break;
            case Type::Bool:  total += 1; break;
            case Type::Text:  total += 4 + (v.is_null ? 0u : v.text.size()); break;
        }
    }

    // Zero-init so null slots have deterministic bytes.
    std::vector<char> out(total, 0);

    // Null bitmap.
    for (size_t i = 0; i < n; ++i) {
        if (vals[i].is_null) {
            out[i / 8] |= static_cast<char>(1u << (i % 8));
        }
    }

    // Payload.
    char* p = out.data() + bitmap_bytes;
    for (size_t i = 0; i < n; ++i) {
        const auto& col = s.columns[i];
        const auto& v = vals[i];
        switch (col.type) {
            case Type::Int32: {
                if (!v.is_null) std::memcpy(p, &v.i32, 4);
                p += 4;
                break;
            }
            case Type::Int64: {
                if (!v.is_null) std::memcpy(p, &v.i64, 8);
                p += 8;
                break;
            }
            case Type::Bool: {
                if (!v.is_null) {
                    uint8_t x = v.b ? 1 : 0;
                    std::memcpy(p, &x, 1);
                }
                p += 1;
                break;
            }
            case Type::Text: {
                const uint32_t text_len = v.is_null
                    ? 0u
                    : static_cast<uint32_t>(v.text.size());
                std::memcpy(p, &text_len, 4);
                p += 4;
                if (!v.is_null && text_len > 0) {
                    std::memcpy(p, v.text.data(), text_len);
                    p += text_len;
                }
                break;
            }
        }
    }
    return out;
}

std::vector<Value> TupleCodec::decode(const Schema& s, const char* bytes, size_t len) {
    const size_t n = s.columns.size();
    const size_t bitmap_bytes = (n + 7) / 8;

    if (len < bitmap_bytes) {
        throw std::runtime_error(
            "TupleCodec::decode: input shorter than null bitmap");
    }

    const char* end = bytes + len;
    const char* p   = bytes + bitmap_bytes;

    std::vector<Value> out;
    out.reserve(n);

    for (size_t i = 0; i < n; ++i) {
        const auto& col = s.columns[i];
        const bool is_null =
            (static_cast<unsigned char>(bytes[i / 8]) >> (i % 8)) & 1u;

        Value v;
        v.type = col.type;
        v.is_null = is_null;

        switch (col.type) {
            case Type::Int32: {
                if (end - p < 4) {
                    throw std::runtime_error("TupleCodec::decode: truncated Int32");
                }
                if (!is_null) std::memcpy(&v.i32, p, 4);
                p += 4;
                break;
            }
            case Type::Int64: {
                if (end - p < 8) {
                    throw std::runtime_error("TupleCodec::decode: truncated Int64");
                }
                if (!is_null) std::memcpy(&v.i64, p, 8);
                p += 8;
                break;
            }
            case Type::Bool: {
                if (end - p < 1) {
                    throw std::runtime_error("TupleCodec::decode: truncated Bool");
                }
                if (!is_null) {
                    uint8_t x;
                    std::memcpy(&x, p, 1);
                    v.b = (x != 0);
                }
                p += 1;
                break;
            }
            case Type::Text: {
                if (end - p < 4) {
                    throw std::runtime_error(
                        "TupleCodec::decode: truncated Text length prefix");
                }
                uint32_t text_len;
                std::memcpy(&text_len, p, 4);
                p += 4;
                if (static_cast<size_t>(end - p) < text_len) {
                    throw std::runtime_error(
                        "TupleCodec::decode: truncated Text payload");
                }
                if (!is_null) v.text.assign(p, text_len);
                p += text_len;
                break;
            }
        }
        out.push_back(std::move(v));
    }

    if (p != end) {
        throw std::runtime_error(
            "TupleCodec::decode: trailing bytes after schema");
    }
    return out;
}
