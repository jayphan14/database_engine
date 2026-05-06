#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "tests/vendor/doctest.h"

#include "src/sql/parser.h"

#include <stdexcept>
#include <string>
#include <variant>

// Most cases here exercise the SELECT path — parse the statement and
// pull the SELECT alternative out, asserting the variant kind matches.
static SelectQuery parse(const std::string& sql) {
    Parser p(sql);
    return std::get<SelectQuery>(p.parse());
}

static CreateTableStmt parseCreate(const std::string& sql) {
    Parser p(sql);
    return std::get<CreateTableStmt>(p.parse());
}

static InsertStmt parseInsert(const std::string& sql) {
    Parser p(sql);
    return std::get<InsertStmt>(p.parse());
}

TEST_CASE("opToString covers every operator") {
    CHECK(std::string(opToString(Op::Eq))  == "=");
    CHECK(std::string(opToString(Op::Neq)) == "!=");
    CHECK(std::string(opToString(Op::Lt))  == "<");
    CHECK(std::string(opToString(Op::Gt))  == ">");
    CHECK(std::string(opToString(Op::Leq)) == "<=");
    CHECK(std::string(opToString(Op::Geq)) == ">=");
}

TEST_CASE("SELECT with explicit columns and numeric WHERE") {
    auto q = parse("SELECT id, name FROM users WHERE age > 18");

    CHECK_FALSE(q.select_all);
    CHECK(q.columns == std::vector<std::string>{"id", "name"});
    CHECK(q.table == "users");
    CHECK(q.joins.empty());
    REQUIRE(q.where.has_value());
    CHECK(q.where->column == "age");
    CHECK(q.where->op == Op::Gt);
    CHECK(q.where->value == "18");
    CHECK_FALSE(q.where->value_is_string);
}

TEST_CASE("SELECT * with no WHERE") {
    auto q = parse("SELECT * FROM products");

    CHECK(q.select_all);
    CHECK(q.columns.empty());
    CHECK(q.table == "products");
    CHECK_FALSE(q.where.has_value());
}

TEST_CASE("Keywords are case-insensitive") {
    auto q = parse("select email, id from accounts where status != 'banned'");

    CHECK(q.columns == std::vector<std::string>{"email", "id"});
    CHECK(q.table == "accounts");
    REQUIRE(q.where.has_value());
    CHECK(q.where->op == Op::Neq);
    CHECK(q.where->value == "banned");
    CHECK(q.where->value_is_string);
}

TEST_CASE("Single JOIN with qualified column refs") {
    auto q = parse("SELECT u.id, u.name, p.title FROM users JOIN posts ON u.id = p.user_id");

    CHECK(q.columns == std::vector<std::string>{"u.id", "u.name", "p.title"});
    CHECK(q.table == "users");
    REQUIRE(q.joins.size() == 1);
    CHECK(q.joins[0].table == "posts");
    CHECK(q.joins[0].left  == "u.id");
    CHECK(q.joins[0].right == "p.user_id");
    CHECK_FALSE(q.where.has_value());
}

TEST_CASE("Multiple JOINs followed by WHERE") {
    auto q = parse("SELECT * FROM a JOIN b ON a.x = b.x JOIN c ON b.y = c.y WHERE c.z > 0");

    CHECK(q.select_all);
    CHECK(q.table == "a");
    REQUIRE(q.joins.size() == 2);
    CHECK(q.joins[0].table == "b");
    CHECK(q.joins[0].left  == "a.x");
    CHECK(q.joins[0].right == "b.x");
    CHECK(q.joins[1].table == "c");
    CHECK(q.joins[1].left  == "b.y");
    CHECK(q.joins[1].right == "c.y");
    REQUIRE(q.where.has_value());
    CHECK(q.where->column == "c.z");
    CHECK(q.where->op == Op::Gt);
    CHECK(q.where->value == "0");
    CHECK_FALSE(q.where->value_is_string);
}

TEST_CASE("All comparison operators parse in WHERE") {
    CHECK(parse("SELECT * FROM t WHERE a = 1").where->op  == Op::Eq);
    CHECK(parse("SELECT * FROM t WHERE a != 1").where->op == Op::Neq);
    CHECK(parse("SELECT * FROM t WHERE a < 1").where->op  == Op::Lt);
    CHECK(parse("SELECT * FROM t WHERE a > 1").where->op  == Op::Gt);
    CHECK(parse("SELECT * FROM t WHERE a <= 1").where->op == Op::Leq);
    CHECK(parse("SELECT * FROM t WHERE a >= 1").where->op == Op::Geq);
}

TEST_CASE("Missing column list after SELECT throws") {
    CHECK_THROWS_AS(parse("SELECT FROM users"), std::runtime_error);
}

TEST_CASE("Trailing tokens after a complete statement throw") {
    CHECK_THROWS_AS(parse("SELECT name FROM users,posts"), std::runtime_error);
}

TEST_CASE("Unterminated string literal throws") {
    CHECK_THROWS_AS(parse("SELECT * FROM t WHERE name = 'unterminated"), std::runtime_error);
}

TEST_CASE("Unexpected character throws") {
    CHECK_THROWS_AS(parse("SELECT * FROM t WHERE a @ 1"), std::runtime_error);
}

TEST_CASE("Lone '!' without '=' throws") {
    CHECK_THROWS_AS(parse("SELECT * FROM t WHERE a ! 1"), std::runtime_error);
}

TEST_CASE("Non-equality operator in JOIN ON throws") {
    CHECK_THROWS_AS(parse("SELECT * FROM a JOIN b ON a.x < b.x"), std::runtime_error);
}

TEST_CASE("WHERE with non-literal RHS throws") {
    CHECK_THROWS_AS(parse("SELECT * FROM t WHERE a = b"), std::runtime_error);
}

TEST_CASE("Empty input throws") {
    CHECK_THROWS_AS(parse(""), std::runtime_error);
}

// ---- CREATE TABLE -------------------------------------------------------

TEST_CASE("CREATE TABLE with mixed types and one NOT NULL constraint") {
    auto s = parseCreate(
        "CREATE TABLE users (id INT NOT NULL, name TEXT, age INT)");

    CHECK(s.table == "users");
    REQUIRE(s.columns.size() == 3);

    CHECK(s.columns[0].name      == "id");
    CHECK(s.columns[0].type_name == "INT");
    CHECK(s.columns[0].nullable  == false);

    CHECK(s.columns[1].name      == "name");
    CHECK(s.columns[1].type_name == "TEXT");
    CHECK(s.columns[1].nullable  == true);

    CHECK(s.columns[2].name      == "age");
    CHECK(s.columns[2].type_name == "INT");
    CHECK(s.columns[2].nullable  == true);
}

TEST_CASE("CREATE TABLE preserves the surface form of the type keyword") {
    // The analyzer is responsible for mapping these; the parser just
    // hands them through verbatim. Lower-case input is preserved too.
    auto s = parseCreate(
        "create table t (a Bigint, b boolean not null, c text)");
    REQUIRE(s.columns.size() == 3);
    CHECK(s.columns[0].type_name == "Bigint");
    CHECK(s.columns[1].type_name == "boolean");
    CHECK(s.columns[1].nullable  == false);
    CHECK(s.columns[2].type_name == "text");
}

TEST_CASE("CREATE TABLE with empty column list throws") {
    CHECK_THROWS_AS(parseCreate("CREATE TABLE t ()"), std::runtime_error);
}

TEST_CASE("CREATE TABLE without parentheses throws") {
    CHECK_THROWS_AS(
        parseCreate("CREATE TABLE t id INT"), std::runtime_error);
}

TEST_CASE("CREATE TABLE with NOT but no NULL throws") {
    CHECK_THROWS_AS(
        parseCreate("CREATE TABLE t (id INT NOT)"), std::runtime_error);
}

// ---- INSERT -------------------------------------------------------------

TEST_CASE("INSERT INTO with no column list and a single row") {
    auto s = parseInsert("INSERT INTO users VALUES (1, 'alice', 30)");

    CHECK(s.table == "users");
    CHECK(s.columns.empty());
    REQUIRE(s.rows.size() == 1);
    REQUIRE(s.rows[0].size() == 3);

    CHECK(s.rows[0][0].text == "1");
    CHECK_FALSE(s.rows[0][0].is_string);
    CHECK_FALSE(s.rows[0][0].is_null);

    CHECK(s.rows[0][1].text == "alice");
    CHECK(s.rows[0][1].is_string);

    CHECK(s.rows[0][2].text == "30");
    CHECK_FALSE(s.rows[0][2].is_string);
}

TEST_CASE("INSERT with explicit column list and multiple rows") {
    auto s = parseInsert(
        "INSERT INTO users (id, name) VALUES (1, 'alice'), (2, 'bob')");

    CHECK(s.columns == std::vector<std::string>{"id", "name"});
    REQUIRE(s.rows.size() == 2);
    CHECK(s.rows[0][1].text == "alice");
    CHECK(s.rows[1][1].text == "bob");
}

TEST_CASE("INSERT with NULL literal carries the is_null flag") {
    auto s = parseInsert("INSERT INTO t VALUES (1, NULL, 'x')");
    REQUIRE(s.rows.size() == 1);
    REQUIRE(s.rows[0].size() == 3);

    CHECK_FALSE(s.rows[0][0].is_null);
    CHECK(s.rows[0][1].is_null);
    CHECK(s.rows[0][1].text.empty());
    CHECK(s.rows[0][1].is_string == false);
    CHECK_FALSE(s.rows[0][2].is_null);
    CHECK(s.rows[0][2].is_string);
}

TEST_CASE("INSERT keywords are case-insensitive") {
    auto s = parseInsert("insert into t values (1)");
    CHECK(s.table == "t");
    REQUIRE(s.rows.size() == 1);
    CHECK(s.rows[0][0].text == "1");
}

TEST_CASE("INSERT without VALUES throws") {
    CHECK_THROWS_AS(
        parseInsert("INSERT INTO t (id) (1)"), std::runtime_error);
}

TEST_CASE("INSERT with empty VALUES row throws") {
    CHECK_THROWS_AS(
        parseInsert("INSERT INTO t VALUES ()"), std::runtime_error);
}

TEST_CASE("INSERT with no rows after VALUES throws") {
    CHECK_THROWS_AS(
        parseInsert("INSERT INTO t VALUES"), std::runtime_error);
}

TEST_CASE("INSERT trailing comma without another row throws") {
    CHECK_THROWS_AS(
        parseInsert("INSERT INTO t VALUES (1),"), std::runtime_error);
}

// ---- top-level dispatch -------------------------------------------------

TEST_CASE("parse() rejects input that doesn't start with a known keyword") {
    Parser p("UPDATE t SET a = 1");
    CHECK_THROWS_AS(p.parse(), std::runtime_error);
}

TEST_CASE("parse() returns the right alternative for each statement kind") {
    {
        Parser p("SELECT * FROM t");
        auto s = p.parse();
        CHECK(std::holds_alternative<SelectQuery>(s));
    }
    {
        Parser p("CREATE TABLE t (a INT)");
        auto s = p.parse();
        CHECK(std::holds_alternative<CreateTableStmt>(s));
    }
    {
        Parser p("INSERT INTO t VALUES (1)");
        auto s = p.parse();
        CHECK(std::holds_alternative<InsertStmt>(s));
    }
}
