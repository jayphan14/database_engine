#include "tests/vendor/doctest.h"

#include "src/sql/tuple.h"

#include <cstdint>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

TEST_CASE("Schema::indexOf finds columns by name") {
    Schema s{{
        {"id",   Type::Int32, false},
        {"name", Type::Bool,  true},
        {"age",  Type::Int64, true},
    }};
    CHECK(s.indexOf("id")   == 0);
    CHECK(s.indexOf("name") == 1);
    CHECK(s.indexOf("age")  == 2);
    CHECK(s.indexOf("missing") == Schema::kNotFound);
}

TEST_CASE("Schema::tupleSize matches the layout: bitmap + sum of widths") {
    Schema s{{
        {"a", Type::Int32, false},   // 4 bytes
        {"b", Type::Int64, true},    // 8 bytes
        {"c", Type::Bool,  false},   // 1 byte
    }};
    // bitmap: ceil(3/8) = 1
    // payload: 4 + 8 + 1 = 13
    CHECK(s.tupleSize() == 1 + 13);
}

TEST_CASE("Schema::tupleSize counts a full bitmap byte every 8 columns") {
    Schema s;
    for (int i = 0; i < 9; ++i) {
        s.columns.push_back({"c" + std::to_string(i), Type::Bool, true});
    }
    // 9 columns → bitmap = 2 bytes; 9 bools → 9 bytes payload.
    CHECK(s.tupleSize() == 2 + 9);
}

TEST_CASE("Value factories build values of the expected kind") {
    auto i = Value::Int32(42);
    CHECK(i.type == Type::Int32);
    CHECK_FALSE(i.is_null);
    CHECK(i.i32 == 42);

    auto j = Value::Int64(0x0123456789ABCDEFLL);
    CHECK(j.type == Type::Int64);
    CHECK(j.i64 == 0x0123456789ABCDEFLL);

    auto t = Value::Bool(true);
    CHECK(t.type == Type::Bool);
    CHECK(t.b == true);

    auto n = Value::Null(Type::Int32);
    CHECK(n.type == Type::Int32);
    CHECK(n.is_null);
}

TEST_CASE("Value equality compares type, null, and payload") {
    CHECK(Value::Int32(1) == Value::Int32(1));
    CHECK(Value::Int32(1) != Value::Int32(2));
    CHECK(Value::Int32(1) != Value::Int64(1));            // different type
    CHECK(Value::Bool(true) != Value::Bool(false));
    CHECK(Value::Null(Type::Int32) == Value::Null(Type::Int32));
    CHECK(Value::Null(Type::Int32) != Value::Null(Type::Bool));
    CHECK(Value::Null(Type::Int32) != Value::Int32(0));   // null vs zero
}

TEST_CASE("encode/decode round trips a non-null tuple") {
    Schema s{{
        {"id",   Type::Int32, false},
        {"big",  Type::Int64, false},
        {"flag", Type::Bool,  false},
    }};
    std::vector<Value> vals = {
        Value::Int32(0x12345678),
        Value::Int64(static_cast<int64_t>(0xCAFEBABEDEADBEEFLL)),
        Value::Bool(true),
    };

    auto bytes = TupleCodec::encode(s, vals);
    REQUIRE(bytes.size() == s.tupleSize());

    auto back = TupleCodec::decode(s, bytes.data(), bytes.size());
    REQUIRE(back.size() == vals.size());
    for (size_t i = 0; i < vals.size(); ++i) {
        CHECK(back[i] == vals[i]);
    }
}

TEST_CASE("encode/decode round trip with mixed null and non-null columns") {
    Schema s{{
        {"a", Type::Int32, true},
        {"b", Type::Int64, true},
        {"c", Type::Bool,  false},
        {"d", Type::Int32, true},
    }};
    std::vector<Value> vals = {
        Value::Null(Type::Int32),
        Value::Int64(99),
        Value::Bool(false),
        Value::Null(Type::Int32),
    };
    auto bytes = TupleCodec::encode(s, vals);
    auto back  = TupleCodec::decode(s, bytes.data(), bytes.size());
    CHECK(back == vals);
}

TEST_CASE("empty schema round trips an empty tuple") {
    Schema s{};
    auto bytes = TupleCodec::encode(s, {});
    CHECK(bytes.empty());
    // decode with empty bytes works because tupleSize() == 0.
    auto back = TupleCodec::decode(s, bytes.data(), 0);
    CHECK(back.empty());
}

TEST_CASE("encode rejects column count mismatch") {
    Schema s{{{"a", Type::Int32, false}, {"b", Type::Bool, false}}};
    CHECK_THROWS_AS(TupleCodec::encode(s, {Value::Int32(1)}), std::runtime_error);
}

TEST_CASE("encode rejects mismatched value type") {
    Schema s{{{"a", Type::Int32, false}}};
    CHECK_THROWS_AS(TupleCodec::encode(s, {Value::Int64(1)}), std::runtime_error);
    CHECK_THROWS_AS(TupleCodec::encode(s, {Value::Bool(false)}), std::runtime_error);
}

TEST_CASE("encode rejects null in a non-nullable column") {
    Schema s{{{"a", Type::Int32, false}}};
    CHECK_THROWS_AS(TupleCodec::encode(s, {Value::Null(Type::Int32)}), std::runtime_error);
}

TEST_CASE("tupleSize is undefined when the schema contains Text columns") {
    // tupleSize is for fixed-only schemas; Text is variable-length, so its
    // size depends on the actual values. Throw rather than guess.
    Schema s{{{"a", Type::Text, true}}};
    CHECK_THROWS_AS(s.tupleSize(), std::runtime_error);
}

TEST_CASE("Text round trips a non-null value of varying lengths") {
    Schema s{{{"name", Type::Text, false}}};

    for (const std::string& sample : {std::string(""),
                                      std::string("hi"),
                                      std::string("a longer text value"),
                                      std::string(1000, 'x')}) {
        std::vector<Value> vals = {Value::Text(sample)};
        auto bytes = TupleCodec::encode(s, vals);
        // Bitmap (1 byte) + length prefix (4 bytes) + payload.
        REQUIRE(bytes.size() == 1 + 4 + sample.size());
        auto back = TupleCodec::decode(s, bytes.data(), bytes.size());
        REQUIRE(back.size() == 1);
        CHECK_FALSE(back[0].is_null);
        CHECK(back[0].text == sample);
    }
}

TEST_CASE("Text distinguishes null from empty string via the bitmap") {
    Schema s{{{"name", Type::Text, true}}};

    // Empty string: not null, length 0.
    auto e_bytes = TupleCodec::encode(s, {Value::Text("")});
    auto e_back  = TupleCodec::decode(s, e_bytes.data(), e_bytes.size());
    CHECK_FALSE(e_back[0].is_null);
    CHECK(e_back[0].text == "");

    // Null: bitmap bit set, length still 0.
    auto n_bytes = TupleCodec::encode(s, {Value::Null(Type::Text)});
    auto n_back  = TupleCodec::decode(s, n_bytes.data(), n_bytes.size());
    CHECK(n_back[0].is_null);

    // Same byte length, different bitmap.
    REQUIRE(e_bytes.size() == n_bytes.size());
    CHECK(e_bytes[0] != n_bytes[0]);
}

TEST_CASE("mixed schema with fixed and Text columns round-trips") {
    Schema s{{
        {"id",     Type::Int32, false},
        {"name",   Type::Text,  false},
        {"score",  Type::Int64, true},
        {"label",  Type::Text,  true},
        {"active", Type::Bool,  false},
    }};
    std::vector<Value> vals = {
        Value::Int32(7),
        Value::Text("alice"),
        Value::Null(Type::Int64),
        Value::Text(""),                 // non-null empty text
        Value::Bool(true),
    };

    auto bytes = TupleCodec::encode(s, vals);
    auto back  = TupleCodec::decode(s, bytes.data(), bytes.size());
    CHECK(back == vals);
}

TEST_CASE("decode rejects truncated Text length prefix or payload") {
    Schema s{{{"name", Type::Text, false}}};
    auto good = TupleCodec::encode(s, {Value::Text("hello")});

    // Lop off the payload — length says 5 but only 3 bytes follow.
    auto truncated_payload = good;
    truncated_payload.resize(truncated_payload.size() - 2);
    CHECK_THROWS_AS(TupleCodec::decode(s, truncated_payload.data(),
                                       truncated_payload.size()),
                    std::runtime_error);

    // Also lop off most of the length prefix itself.
    auto truncated_prefix = good;
    truncated_prefix.resize(2);  // bitmap byte + 1 byte of length
    CHECK_THROWS_AS(TupleCodec::decode(s, truncated_prefix.data(),
                                       truncated_prefix.size()),
                    std::runtime_error);
}

TEST_CASE("decode rejects wrong byte length") {
    Schema s{{{"a", Type::Int32, false}}};
    auto good = TupleCodec::encode(s, {Value::Int32(1)});
    CHECK_THROWS_AS(TupleCodec::decode(s, good.data(), good.size() - 1), std::runtime_error);
    CHECK_THROWS_AS(TupleCodec::decode(s, good.data(), good.size() + 1), std::runtime_error);
}

TEST_CASE("randomized round-trip stress: 200 random schemas/tuples") {
    std::mt19937 rng(0xBEEFu);
    std::uniform_int_distribution<int> n_cols_dist(1, 8);
    std::uniform_int_distribution<int> type_dist(0, 3);  // includes Text
    std::uniform_int_distribution<int> coin(0, 1);
    std::uniform_int_distribution<int> text_len_dist(0, 40);
    std::uniform_int_distribution<int> byte_dist(0, 255);

    auto type_for = [](int t) {
        switch (t) {
            case 0:  return Type::Int32;
            case 1:  return Type::Int64;
            case 2:  return Type::Bool;
            default: return Type::Text;
        }
    };

    for (int trial = 0; trial < 200; ++trial) {
        Schema s;
        const int n_cols = n_cols_dist(rng);
        for (int i = 0; i < n_cols; ++i) {
            s.columns.push_back({"c" + std::to_string(i),
                                 type_for(type_dist(rng)),
                                 coin(rng) != 0});
        }

        std::vector<Value> vals;
        for (const auto& col : s.columns) {
            const bool make_null = col.nullable && coin(rng) == 0;
            if (make_null) {
                vals.push_back(Value::Null(col.type));
                continue;
            }
            switch (col.type) {
                case Type::Int32:
                    vals.push_back(Value::Int32(static_cast<int32_t>(rng())));
                    break;
                case Type::Int64: {
                    const int64_t hi = static_cast<int64_t>(rng()) << 32;
                    const int64_t lo = static_cast<int64_t>(rng());
                    vals.push_back(Value::Int64(hi | lo));
                    break;
                }
                case Type::Bool:
                    vals.push_back(Value::Bool(coin(rng) != 0));
                    break;
                case Type::Text: {
                    std::string t(text_len_dist(rng), '\0');
                    for (auto& c : t) c = static_cast<char>(byte_dist(rng));
                    vals.push_back(Value::Text(std::move(t)));
                    break;
                }
            }
        }

        auto bytes = TupleCodec::encode(s, vals);
        auto back = TupleCodec::decode(s, bytes.data(), bytes.size());
        REQUIRE(back.size() == vals.size());
        for (size_t i = 0; i < vals.size(); ++i) {
            REQUIRE(back[i] == vals[i]);
        }
    }
}
