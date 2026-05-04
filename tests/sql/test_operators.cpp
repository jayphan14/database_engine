// Direct tests of the Volcano operators in src/sql/operators.{h,cpp}.
// The end-to-end behavior (SQL string in, ExecResult out) is already
// covered by test_executor.cpp; the cases below exercise operator
// state machines and edge cases that are awkward to reach through
// SQL — empty inputs, repeated next-after-end, and the EXPLAIN
// describe() output.

#include "tests/vendor/doctest.h"

#include "src/sql/analyzer.h"
#include "src/sql/catalog.h"
#include "src/sql/operators.h"
#include "src/sql/plan_node.h"
#include "src/sql/tuple.h"
#include "src/storage/buffer_pool.h"
#include "src/storage/disk_manager.h"
#include "src/storage/heap_file.h"
#include "tests/test_util.h"

#include <cstdint>
#include <memory>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace {

Schema usersSchema() {
    return Schema{{
        {"id",   Type::Int32, false},
        {"name", Type::Text,  false},
        {"age",  Type::Int32, false},
    }};
}

Schema postsSchema() {
    return Schema{{
        {"id",      Type::Int32, false},
        {"user_id", Type::Int32, false},
    }};
}

void seedUsers(BufferPool& bp, const Catalog::TableInfo& info) {
    const std::vector<std::tuple<int32_t, std::string, int32_t>> rows = {
        {1, "alice", 30},
        {2, "bob",   25},
        {3, "carol", 40},
    };
    HeapFile hf(&bp, info.root_page);
    for (const auto& [id, name, age] : rows) {
        const auto bytes = TupleCodec::encode(info.schema, {
            Value::Int32(id), Value::Text(name), Value::Int32(age),
        });
        hf.insert(bytes.data(), bytes.size());
    }
}

void seedPosts(BufferPool& bp, const Catalog::TableInfo& info) {
    // Two posts for user 1, one for user 3, none for user 2.
    const std::vector<std::pair<int32_t, int32_t>> rows = {
        {100, 1}, {101, 1}, {102, 3},
    };
    HeapFile hf(&bp, info.root_page);
    for (auto [id, user_id] : rows) {
        const auto bytes = TupleCodec::encode(info.schema, {
            Value::Int32(id), Value::Int32(user_id),
        });
        hf.insert(bytes.data(), bytes.size());
    }
}

// Build the BoundExpr for `users.age > threshold` (table_index=0,
// column_index=2 against usersSchema()) by hand — bypasses parser +
// analyzer so the test can construct a Filter directly.
BoundExpr ageGreaterThan(int32_t threshold) {
    auto bop = std::make_unique<BoundBinaryOp>();
    bop->op  = Op::Gt;
    bop->lhs = BoundColumnRef{0, 2, Type::Int32};
    bop->rhs = BoundLiteral{Value::Int32(threshold), Type::Int32};
    bop->result_type = Type::Bool;
    return BoundExpr{std::move(bop)};
}

}  // namespace

TEST_CASE("SeqScan emits every row in heap-file order") {
    TempFile tf;
    DiskManager dm(tf.path());
    BufferPool bp(8, &dm);
    Catalog cat = Catalog::create(&bp);
    cat.createTable("users", usersSchema());
    seedUsers(bp, *cat.getTable("users"));

    SeqScan scan(&bp, cat.getTable("users"), /*table_index=*/0, /*total=*/1);
    scan.open();

    std::vector<std::string> names;
    while (auto row = scan.next()) {
        REQUIRE(row->size() == 1);                  // total_tables = 1
        REQUIRE_FALSE((*row)[0].empty());           // slot populated
        names.push_back((*row)[0][1].text);         // schema col 1 = name
    }
    scan.close();

    CHECK(names == std::vector<std::string>{"alice", "bob", "carol"});
}

TEST_CASE("SeqScan on an empty table emits nothing") {
    TempFile tf;
    DiskManager dm(tf.path());
    BufferPool bp(8, &dm);
    Catalog cat = Catalog::create(&bp);
    cat.createTable("users", usersSchema());

    SeqScan scan(&bp, cat.getTable("users"), 0, 1);
    scan.open();
    CHECK_FALSE(scan.next().has_value());
    scan.close();
}

TEST_CASE("SeqScan returns nullopt repeatedly after exhaustion") {
    TempFile tf;
    DiskManager dm(tf.path());
    BufferPool bp(8, &dm);
    Catalog cat = Catalog::create(&bp);
    cat.createTable("users", usersSchema());
    seedUsers(bp, *cat.getTable("users"));

    SeqScan scan(&bp, cat.getTable("users"), 0, 1);
    scan.open();
    int yielded = 0;
    while (scan.next()) ++yielded;
    REQUIRE(yielded == 3);
    // Past-the-end calls must keep returning nullopt, not throw or yield.
    CHECK_FALSE(scan.next().has_value());
    CHECK_FALSE(scan.next().has_value());
    scan.close();
}

TEST_CASE("SeqScan populates only its assigned slot in the wide row") {
    TempFile tf;
    DiskManager dm(tf.path());
    BufferPool bp(8, &dm);
    Catalog cat = Catalog::create(&bp);
    cat.createTable("posts", postsSchema());
    seedPosts(bp, *cat.getTable("posts"));

    // Pretend we're table_index=1 in a 3-table FROM list.
    SeqScan scan(&bp, cat.getTable("posts"), /*table_index=*/1, /*total=*/3);
    scan.open();

    auto row = scan.next();
    REQUIRE(row.has_value());
    REQUIRE(row->size() == 3);
    CHECK((*row)[0].empty());
    CHECK_FALSE((*row)[1].empty());
    CHECK((*row)[2].empty());
    scan.close();
}

TEST_CASE("Filter drops rows whose predicate is false") {
    TempFile tf;
    DiskManager dm(tf.path());
    BufferPool bp(8, &dm);
    Catalog cat = Catalog::create(&bp);
    cat.createTable("users", usersSchema());
    seedUsers(bp, *cat.getTable("users"));

    auto scan = std::make_unique<SeqScan>(&bp, cat.getTable("users"), 0, 1);
    Filter filter(std::move(scan), ageGreaterThan(25));
    filter.open();

    std::vector<std::string> names;
    while (auto row = filter.next()) {
        names.push_back((*row)[0][1].text);
    }
    filter.close();

    CHECK(names == std::vector<std::string>{"alice", "carol"});
}

TEST_CASE("Filter with always-true predicate is a pass-through") {
    TempFile tf;
    DiskManager dm(tf.path());
    BufferPool bp(8, &dm);
    Catalog cat = Catalog::create(&bp);
    cat.createTable("users", usersSchema());
    seedUsers(bp, *cat.getTable("users"));

    auto scan = std::make_unique<SeqScan>(&bp, cat.getTable("users"), 0, 1);
    Filter filter(std::move(scan), ageGreaterThan(0));
    filter.open();
    int count = 0;
    while (filter.next()) ++count;
    filter.close();
    CHECK(count == 3);
}

TEST_CASE("NestedLoopJoin overlays disjoint slots and matches on equality") {
    TempFile tf;
    DiskManager dm(tf.path());
    BufferPool bp(8, &dm);
    Catalog cat = Catalog::create(&bp);
    cat.createTable("users", usersSchema());
    cat.createTable("posts", postsSchema());
    seedUsers(bp, *cat.getTable("users"));
    seedPosts(bp, *cat.getTable("posts"));

    auto left  = std::make_unique<SeqScan>(&bp, cat.getTable("users"), 0, 2);
    auto right = std::make_unique<SeqScan>(&bp, cat.getTable("posts"), 1, 2);

    // ON users.id (col 0) = posts.user_id (col 1).
    BoundJoin on{
        BoundColumnRef{0, 0, Type::Int32},
        BoundColumnRef{1, 1, Type::Int32},
    };
    NestedLoopJoin nlj(std::move(left), std::move(right), on);
    nlj.open();

    std::vector<std::pair<std::string, int32_t>> matches;
    while (auto row = nlj.next()) {
        REQUIRE(row->size() == 2);
        REQUIRE_FALSE((*row)[0].empty());
        REQUIRE_FALSE((*row)[1].empty());
        matches.emplace_back((*row)[0][1].text, (*row)[1][0].i32);
    }
    nlj.close();

    // alice (id=1) → posts 100 and 101; carol (id=3) → post 102. bob has none.
    REQUIRE(matches.size() == 3);
    CHECK(matches[0] == std::pair<std::string, int32_t>{"alice", 100});
    CHECK(matches[1] == std::pair<std::string, int32_t>{"alice", 101});
    CHECK(matches[2] == std::pair<std::string, int32_t>{"carol", 102});
}

TEST_CASE("NestedLoopJoin with an empty right side emits nothing") {
    TempFile tf;
    DiskManager dm(tf.path());
    BufferPool bp(8, &dm);
    Catalog cat = Catalog::create(&bp);
    cat.createTable("users", usersSchema());
    cat.createTable("posts", postsSchema());
    seedUsers(bp, *cat.getTable("users"));
    // posts left empty.

    auto left  = std::make_unique<SeqScan>(&bp, cat.getTable("users"), 0, 2);
    auto right = std::make_unique<SeqScan>(&bp, cat.getTable("posts"), 1, 2);
    BoundJoin on{
        BoundColumnRef{0, 0, Type::Int32},
        BoundColumnRef{1, 1, Type::Int32},
    };
    NestedLoopJoin nlj(std::move(left), std::move(right), on);
    nlj.open();
    CHECK_FALSE(nlj.next().has_value());
    nlj.close();
}

TEST_CASE("NestedLoopJoin with an empty left side emits nothing") {
    TempFile tf;
    DiskManager dm(tf.path());
    BufferPool bp(8, &dm);
    Catalog cat = Catalog::create(&bp);
    cat.createTable("users", usersSchema());
    cat.createTable("posts", postsSchema());
    seedPosts(bp, *cat.getTable("posts"));
    // users left empty.

    auto left  = std::make_unique<SeqScan>(&bp, cat.getTable("users"), 0, 2);
    auto right = std::make_unique<SeqScan>(&bp, cat.getTable("posts"), 1, 2);
    BoundJoin on{
        BoundColumnRef{0, 0, Type::Int32},
        BoundColumnRef{1, 1, Type::Int32},
    };
    NestedLoopJoin nlj(std::move(left), std::move(right), on);
    nlj.open();
    CHECK_FALSE(nlj.next().has_value());
    nlj.close();
}

TEST_CASE("describe() prints an indented tree of operator names") {
    TempFile tf;
    DiskManager dm(tf.path());
    BufferPool bp(8, &dm);
    Catalog cat = Catalog::create(&bp);
    cat.createTable("users", usersSchema());
    cat.createTable("posts", postsSchema());

    auto left  = std::make_unique<SeqScan>(&bp, cat.getTable("users"), 0, 2);
    auto right = std::make_unique<SeqScan>(&bp, cat.getTable("posts"), 1, 2);
    BoundJoin on{
        BoundColumnRef{0, 0, Type::Int32},
        BoundColumnRef{1, 1, Type::Int32},
    };
    auto nlj = std::make_unique<NestedLoopJoin>(
        std::move(left), std::move(right), on);
    Filter filter(std::move(nlj), ageGreaterThan(20));

    std::ostringstream oss;
    filter.describe(oss, 0);
    const std::string out = oss.str();

    // Don't pin to exact whitespace — just verify the structure.
    CHECK(out.find("Filter")           != std::string::npos);
    CHECK(out.find("NestedLoopJoin")   != std::string::npos);
    CHECK(out.find("SeqScan(users)")   != std::string::npos);
    CHECK(out.find("SeqScan(posts)")   != std::string::npos);

    // Filter sits at indent 0; its child sits deeper. The first
    // non-Filter line should start with at least two spaces.
    const auto nlj_pos = out.find("NestedLoopJoin");
    REQUIRE(nlj_pos != std::string::npos);
    const auto line_start = out.rfind('\n', nlj_pos);
    const std::size_t indent_start = (line_start == std::string::npos) ? 0 : line_start + 1;
    CHECK(nlj_pos - indent_start >= 2);
}
