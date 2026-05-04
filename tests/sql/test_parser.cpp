#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "vendor/doctest.h"

#include "src/parser.h"

#include <stdexcept>
#include <string>

static SelectQuery parse(const std::string& sql) {
    Parser p(sql);
    return p.parse();
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
