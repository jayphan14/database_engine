#include "tests/vendor/doctest.h"

#include "src/parser.h"
#include "src/sql/analyzer.h"
#include "src/sql/catalog.h"
#include "src/sql/tuple.h"
#include "src/storage/buffer_pool.h"
#include "src/storage/disk_manager.h"
#include "tests/test_util.h"

#include <stdexcept>
#include <string>
#include <variant>

namespace {

Schema usersSchema() {
    return Schema{{
        {"id",     Type::Int32, false},
        {"name",   Type::Text,  false},
        {"age",    Type::Int32, true},
        {"salary", Type::Int64, false},
        {"active", Type::Bool,  false},
    }};
}

Schema postsSchema() {
    return Schema{{
        {"id",      Type::Int32, false},
        {"title",   Type::Text,  false},
        {"user_id", Type::Int32, false},
    }};
}

// Convenience: parse + analyze in one go.
BoundSelect parseAnalyze(const Analyzer& az, const std::string& sql) {
    Parser p(sql);
    SelectQuery q = p.parse();
    return az.analyze(q);
}

// Pull out a BoundColumnRef from a select_list slot, asserting it's that
// alternative. Lets test cases stay terse.
const BoundColumnRef& asColumn(const BoundExpr& e) {
    return std::get<BoundColumnRef>(e);
}

// Pull out the BoundBinaryOp from a BoundExpr.
const BoundBinaryOp& asBinaryOp(const BoundExpr& e) {
    return *std::get<std::unique_ptr<BoundBinaryOp>>(e);
}

}  // namespace

TEST_CASE("SELECT * expands every column of the FROM table") {
    TempFile tf;
    DiskManager dm(tf.path());
    BufferPool bp(4, &dm);
    Catalog cat = Catalog::create(&bp);
    cat.createTable("users", usersSchema());
    Analyzer az(cat);

    auto b = parseAnalyze(az, "SELECT * FROM users");

    REQUIRE(b.from_tables.size() == 1);
    CHECK(b.from_tables[0]->name == "users");
    CHECK(b.select_all);
    REQUIRE(b.select_list.size() == 5);
    for (size_t i = 0; i < 5; ++i) {
        const auto& c = asColumn(b.select_list[i]);
        CHECK(c.table_index == 0);
        CHECK(c.column_index == i);
    }
    CHECK_FALSE(b.where.has_value());
}

TEST_CASE("SELECT with explicit columns resolves names to indices") {
    TempFile tf;
    DiskManager dm(tf.path());
    BufferPool bp(4, &dm);
    Catalog cat = Catalog::create(&bp);
    cat.createTable("users", usersSchema());
    Analyzer az(cat);

    auto b = parseAnalyze(az, "SELECT name, salary FROM users");

    CHECK_FALSE(b.select_all);
    REQUIRE(b.select_list.size() == 2);
    CHECK(asColumn(b.select_list[0]).column_index == 1);
    CHECK(asColumn(b.select_list[0]).result_type == Type::Text);
    CHECK(asColumn(b.select_list[1]).column_index == 3);
    CHECK(asColumn(b.select_list[1]).result_type == Type::Int64);
}

TEST_CASE("WHERE on Int32 column with a numeric literal binds correctly") {
    TempFile tf;
    DiskManager dm(tf.path());
    BufferPool bp(4, &dm);
    Catalog cat = Catalog::create(&bp);
    cat.createTable("users", usersSchema());
    Analyzer az(cat);

    auto b = parseAnalyze(az, "SELECT * FROM users WHERE age > 18");

    REQUIRE(b.where.has_value());
    const auto& bin = asBinaryOp(*b.where);
    CHECK(bin.op == Op::Gt);
    CHECK(bin.result_type == Type::Bool);

    const auto& col = std::get<BoundColumnRef>(bin.lhs);
    CHECK(col.column_index == 2);     // "age"
    CHECK(col.result_type == Type::Int32);

    const auto& lit = std::get<BoundLiteral>(bin.rhs);
    CHECK(lit.result_type == Type::Int32);
    CHECK(lit.value.i32 == 18);
}

TEST_CASE("WHERE with a string literal targets a Text column") {
    TempFile tf;
    DiskManager dm(tf.path());
    BufferPool bp(4, &dm);
    Catalog cat = Catalog::create(&bp);
    cat.createTable("users", usersSchema());
    Analyzer az(cat);

    auto b = parseAnalyze(az, "SELECT * FROM users WHERE name = 'alice'");

    REQUIRE(b.where.has_value());
    const auto& bin = asBinaryOp(*b.where);
    CHECK(bin.op == Op::Eq);
    CHECK(std::get<BoundLiteral>(bin.rhs).value.text == "alice");
    CHECK(std::get<BoundLiteral>(bin.rhs).result_type == Type::Text);
}

TEST_CASE("WHERE on Int64 column parses the literal as Int64") {
    TempFile tf;
    DiskManager dm(tf.path());
    BufferPool bp(4, &dm);
    Catalog cat = Catalog::create(&bp);
    cat.createTable("users", usersSchema());
    Analyzer az(cat);

    auto b = parseAnalyze(az, "SELECT * FROM users WHERE salary >= 50000");
    const auto& bin = asBinaryOp(*b.where);
    CHECK(bin.op == Op::Geq);
    const auto& lit = std::get<BoundLiteral>(bin.rhs);
    CHECK(lit.result_type == Type::Int64);
    CHECK(lit.value.i64 == 50000);
}

TEST_CASE("WHERE on Bool column accepts 0 / 1 as the literal") {
    TempFile tf;
    DiskManager dm(tf.path());
    BufferPool bp(4, &dm);
    Catalog cat = Catalog::create(&bp);
    cat.createTable("users", usersSchema());
    Analyzer az(cat);

    auto b = parseAnalyze(az, "SELECT * FROM users WHERE active = 1");
    const auto& bin = asBinaryOp(*b.where);
    const auto& lit = std::get<BoundLiteral>(bin.rhs);
    CHECK(lit.result_type == Type::Bool);
    CHECK(lit.value.b == true);
}

TEST_CASE("Qualified column reference (table.column) resolves") {
    TempFile tf;
    DiskManager dm(tf.path());
    BufferPool bp(4, &dm);
    Catalog cat = Catalog::create(&bp);
    cat.createTable("users", usersSchema());
    Analyzer az(cat);

    auto b = parseAnalyze(az, "SELECT users.id FROM users");
    REQUIRE(b.select_list.size() == 1);
    const auto& col = asColumn(b.select_list[0]);
    CHECK(col.table_index == 0);
    CHECK(col.column_index == 0);
    CHECK(col.result_type == Type::Int32);
}

TEST_CASE("unknown table throws") {
    TempFile tf;
    DiskManager dm(tf.path());
    BufferPool bp(4, &dm);
    Catalog cat = Catalog::create(&bp);
    cat.createTable("users", usersSchema());
    Analyzer az(cat);

    CHECK_THROWS_AS(parseAnalyze(az, "SELECT * FROM missing"),
                    std::runtime_error);
}

TEST_CASE("unknown column throws") {
    TempFile tf;
    DiskManager dm(tf.path());
    BufferPool bp(4, &dm);
    Catalog cat = Catalog::create(&bp);
    cat.createTable("users", usersSchema());
    Analyzer az(cat);

    CHECK_THROWS_AS(parseAnalyze(az, "SELECT bogus FROM users"),
                    std::runtime_error);
    CHECK_THROWS_AS(parseAnalyze(az, "SELECT * FROM users WHERE bogus = 1"),
                    std::runtime_error);
}

TEST_CASE("qualified column with wrong table name throws") {
    TempFile tf;
    DiskManager dm(tf.path());
    BufferPool bp(4, &dm);
    Catalog cat = Catalog::create(&bp);
    cat.createTable("users", usersSchema());
    Analyzer az(cat);

    CHECK_THROWS_AS(parseAnalyze(az, "SELECT posts.id FROM users"),
                    std::runtime_error);
}

TEST_CASE("type-mismatched WHERE throws") {
    TempFile tf;
    DiskManager dm(tf.path());
    BufferPool bp(4, &dm);
    Catalog cat = Catalog::create(&bp);
    cat.createTable("users", usersSchema());
    Analyzer az(cat);

    // Numeric column compared against a string literal.
    CHECK_THROWS_AS(parseAnalyze(az, "SELECT * FROM users WHERE age = 'foo'"),
                    std::runtime_error);
    // Text column compared against a numeric literal.
    CHECK_THROWS_AS(parseAnalyze(az, "SELECT * FROM users WHERE name = 5"),
                    std::runtime_error);
}

TEST_CASE("ordering operator on Text column throws") {
    TempFile tf;
    DiskManager dm(tf.path());
    BufferPool bp(4, &dm);
    Catalog cat = Catalog::create(&bp);
    cat.createTable("users", usersSchema());
    Analyzer az(cat);

    // Even though both sides become Text, < is rejected on Text for now.
    CHECK_THROWS_AS(parseAnalyze(az, "SELECT * FROM users WHERE name < 'm'"),
                    std::runtime_error);
}

TEST_CASE("integer literal that overflows Int32 column throws") {
    TempFile tf;
    DiskManager dm(tf.path());
    BufferPool bp(4, &dm);
    Catalog cat = Catalog::create(&bp);
    cat.createTable("users", usersSchema());
    Analyzer az(cat);

    // 9999999999 > INT32_MAX (~2.1e9) so should be rejected against age (Int32),
    // but a similar literal against salary (Int64) succeeds.
    CHECK_THROWS_AS(parseAnalyze(az, "SELECT * FROM users WHERE age > 9999999999"),
                    std::runtime_error);
    auto b = parseAnalyze(az, "SELECT * FROM users WHERE salary > 9999999999");
    REQUIRE(b.where.has_value());
}

TEST_CASE("Single JOIN resolves both ON columns against the full scope") {
    TempFile tf;
    DiskManager dm(tf.path());
    BufferPool bp(4, &dm);
    Catalog cat = Catalog::create(&bp);
    cat.createTable("users", usersSchema());
    cat.createTable("posts", postsSchema());
    Analyzer az(cat);

    auto b = parseAnalyze(az,
        "SELECT users.name, posts.title FROM users "
        "JOIN posts ON users.id = posts.user_id");

    REQUIRE(b.from_tables.size() == 2);
    CHECK(b.from_tables[0]->name == "users");
    CHECK(b.from_tables[1]->name == "posts");

    REQUIRE(b.joins.size() == 1);
    const auto& jn = b.joins[0];
    CHECK(jn.left.table_index  == 0);   // users
    CHECK(jn.left.column_index == 0);   // id
    CHECK(jn.left.result_type  == Type::Int32);
    CHECK(jn.right.table_index  == 1);  // posts
    CHECK(jn.right.column_index == 2);  // user_id
    CHECK(jn.right.result_type  == Type::Int32);

    REQUIRE(b.select_list.size() == 2);
    CHECK(asColumn(b.select_list[0]).table_index  == 0);   // users.name
    CHECK(asColumn(b.select_list[0]).column_index == 1);
    CHECK(asColumn(b.select_list[1]).table_index  == 1);   // posts.title
    CHECK(asColumn(b.select_list[1]).column_index == 1);
}

TEST_CASE("SELECT * across a JOIN expands every column of every FROM table") {
    TempFile tf;
    DiskManager dm(tf.path());
    BufferPool bp(4, &dm);
    Catalog cat = Catalog::create(&bp);
    cat.createTable("users", usersSchema());
    cat.createTable("posts", postsSchema());
    Analyzer az(cat);

    auto b = parseAnalyze(az,
        "SELECT * FROM users JOIN posts ON users.id = posts.user_id");

    // 5 (users) + 3 (posts) = 8 expanded columns, in table order.
    REQUIRE(b.select_list.size() == 8);
    CHECK(asColumn(b.select_list[0]).table_index == 0);
    CHECK(asColumn(b.select_list[4]).table_index == 0);
    CHECK(asColumn(b.select_list[5]).table_index == 1);
    CHECK(asColumn(b.select_list[7]).table_index == 1);
}

TEST_CASE("WHERE after a JOIN can reference either table") {
    TempFile tf;
    DiskManager dm(tf.path());
    BufferPool bp(4, &dm);
    Catalog cat = Catalog::create(&bp);
    cat.createTable("users", usersSchema());
    cat.createTable("posts", postsSchema());
    Analyzer az(cat);

    auto b = parseAnalyze(az,
        "SELECT users.name FROM users JOIN posts ON users.id = posts.user_id "
        "WHERE posts.title = 'hi'");

    REQUIRE(b.where.has_value());
    const auto& bin = asBinaryOp(*b.where);
    const auto& col = std::get<BoundColumnRef>(bin.lhs);
    CHECK(col.table_index  == 1);  // posts
    CHECK(col.column_index == 1);  // title
}

TEST_CASE("JOIN on an unknown table throws") {
    TempFile tf;
    DiskManager dm(tf.path());
    BufferPool bp(4, &dm);
    Catalog cat = Catalog::create(&bp);
    cat.createTable("users", usersSchema());
    Analyzer az(cat);

    CHECK_THROWS_AS(
        parseAnalyze(az,
            "SELECT * FROM users JOIN posts ON users.id = posts.user_id"),
        std::runtime_error);
}

TEST_CASE("JOIN ON with an unknown column throws") {
    TempFile tf;
    DiskManager dm(tf.path());
    BufferPool bp(4, &dm);
    Catalog cat = Catalog::create(&bp);
    cat.createTable("users", usersSchema());
    cat.createTable("posts", postsSchema());
    Analyzer az(cat);

    CHECK_THROWS_AS(
        parseAnalyze(az,
            "SELECT * FROM users JOIN posts ON users.id = posts.bogus"),
        std::runtime_error);
}

TEST_CASE("JOIN ON with mismatched column types throws") {
    TempFile tf;
    DiskManager dm(tf.path());
    BufferPool bp(4, &dm);
    Catalog cat = Catalog::create(&bp);
    cat.createTable("users", usersSchema());
    cat.createTable("posts", postsSchema());
    Analyzer az(cat);

    // users.name is Text, posts.user_id is Int32.
    CHECK_THROWS_AS(
        parseAnalyze(az,
            "SELECT * FROM users JOIN posts ON users.name = posts.user_id"),
        std::runtime_error);
}

TEST_CASE("Repeating the same table in FROM/JOIN throws (no aliases yet)") {
    TempFile tf;
    DiskManager dm(tf.path());
    BufferPool bp(4, &dm);
    Catalog cat = Catalog::create(&bp);
    cat.createTable("users", usersSchema());
    Analyzer az(cat);

    CHECK_THROWS_AS(
        parseAnalyze(az,
            "SELECT * FROM users JOIN users ON users.id = users.id"),
        std::runtime_error);
}

TEST_CASE("Bare column appearing in two joined tables is ambiguous") {
    TempFile tf;
    DiskManager dm(tf.path());
    BufferPool bp(4, &dm);
    Catalog cat = Catalog::create(&bp);
    cat.createTable("users", usersSchema());
    cat.createTable("posts", postsSchema());
    Analyzer az(cat);

    // Both users and posts have an "id" column; bare "id" must be rejected.
    CHECK_THROWS_AS(
        parseAnalyze(az,
            "SELECT id FROM users JOIN posts ON users.id = posts.user_id"),
        std::runtime_error);
}

TEST_CASE("Multiple chained JOINs all resolve") {
    TempFile tf;
    DiskManager dm(tf.path());
    BufferPool bp(4, &dm);
    Catalog cat = Catalog::create(&bp);
    cat.createTable("a", Schema{{{"x", Type::Int32, false}}});
    cat.createTable("b", Schema{{{"x", Type::Int32, false},
                                  {"y", Type::Int32, false}}});
    cat.createTable("c", Schema{{{"y", Type::Int32, false},
                                  {"z", Type::Int32, false}}});
    Analyzer az(cat);

    auto bs = parseAnalyze(az,
        "SELECT a.x, c.z FROM a JOIN b ON a.x = b.x JOIN c ON b.y = c.y");

    REQUIRE(bs.from_tables.size() == 3);
    REQUIRE(bs.joins.size() == 2);
    CHECK(bs.joins[0].left.table_index  == 0);  // a
    CHECK(bs.joins[0].right.table_index == 1);  // b
    CHECK(bs.joins[1].left.table_index  == 1);  // b
    CHECK(bs.joins[1].right.table_index == 2);  // c
}
