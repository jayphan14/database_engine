#include "tests/vendor/doctest.h"

#include "src/sql/parser.h"
#include "src/sql/analyzer.h"
#include "src/sql/catalog.h"
#include "src/sql/executor.h"
#include "src/sql/tuple.h"
#include "src/storage/buffer_pool.h"
#include "src/storage/disk_manager.h"
#include "src/storage/heap_file.h"
#include "tests/test_util.h"

#include <cstdint>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace {

Schema usersSchema() {
    return Schema{{
        {"id",     Type::Int32, false},
        {"name",   Type::Text,  false},
        {"age",    Type::Int32, false},
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

// Five users seeded in a fixed order so the executor's scan-order output
// is deterministic. Keep insertion order in sync with the row-order
// assertions below.
void seedUsers(BufferPool& bp, const Catalog::TableInfo& info) {
    const std::vector<std::tuple<int32_t, std::string, int32_t, int64_t, bool>> rows = {
        {1, "alice", 30, 80000LL, true},
        {2, "bob",   25, 60000LL, true},
        {3, "carol", 40, 90000LL, false},
        {4, "dave",  19, 30000LL, true},
        {5, "eve",   33, 70000LL, false},
    };
    HeapFile hf(&bp, info.root_page);
    for (const auto& [id, name, age, salary, active] : rows) {
        const auto bytes = TupleCodec::encode(info.schema, {
            Value::Int32(id),
            Value::Text(name),
            Value::Int32(age),
            Value::Int64(salary),
            Value::Bool(active),
        });
        hf.insert(bytes.data(), bytes.size());
    }
}

// Five posts. user_id values are chosen so that user 4 ("dave") has no
// posts — the join tests rely on that gap.
void seedPosts(BufferPool& bp, const Catalog::TableInfo& info) {
    const std::vector<std::tuple<int32_t, std::string, int32_t>> rows = {
        {100, "a1", 1},
        {101, "a2", 1},
        {102, "c1", 3},
        {103, "e1", 5},
        {104, "b1", 2},
    };
    HeapFile hf(&bp, info.root_page);
    for (const auto& [id, title, user_id] : rows) {
        const auto bytes = TupleCodec::encode(info.schema, {
            Value::Int32(id),
            Value::Text(title),
            Value::Int32(user_id),
        });
        hf.insert(bytes.data(), bytes.size());
    }
}

// Convenience: parse + analyze + execute in one shot.
ExecResult run(Catalog& cat, BufferPool& bp, const std::string& sql) {
    Parser p(sql);
    SelectQuery q = std::get<SelectQuery>(p.parse());
    Analyzer az(cat);
    BoundSelect bs = az.analyze(q);
    Executor ex(&bp, &cat);
    return ex.execute(std::move(bs));
}

// Variant of `run` that drives the analyzer's variant entry point — the
// only path through which CREATE TABLE / INSERT can reach the executor.
ExecResult runStatement(Catalog& cat, BufferPool& bp, const std::string& sql) {
    Parser p(sql);
    Analyzer az(cat);
    BoundStatement bound = az.analyzeStatement(p.parse());
    Executor ex(&bp, &cat);
    return ex.execute(std::move(bound));
}

std::vector<std::string> textCol(const ExecResult& r, size_t c) {
    std::vector<std::string> out;
    out.reserve(r.rows.size());
    for (const auto& row : r.rows) out.push_back(row[c].text);
    return out;
}

std::vector<int32_t> i32Col(const ExecResult& r, size_t c) {
    std::vector<int32_t> out;
    out.reserve(r.rows.size());
    for (const auto& row : r.rows) out.push_back(row[c].i32);
    return out;
}

}  // namespace

TEST_CASE("SELECT * returns every row in heap-file scan order") {
    TempFile tf;
    DiskManager dm(tf.path());
    BufferPool bp(8, &dm);
    Catalog cat = Catalog::create(&bp);
    cat.createTable("users", usersSchema());
    seedUsers(bp, *cat.getTable("users"));

    auto r = run(cat, bp, "SELECT * FROM users");
    REQUIRE(r.rows.size() == 5);
    REQUIRE(r.column_names.size() == 5);
    CHECK(r.column_names[0] == "id");
    CHECK(r.column_names[4] == "active");
    CHECK(textCol(r, 1) ==
          std::vector<std::string>{"alice", "bob", "carol", "dave", "eve"});
}

TEST_CASE("Explicit column list projects in the requested order") {
    TempFile tf;
    DiskManager dm(tf.path());
    BufferPool bp(8, &dm);
    Catalog cat = Catalog::create(&bp);
    cat.createTable("users", usersSchema());
    seedUsers(bp, *cat.getTable("users"));

    auto r = run(cat, bp, "SELECT age, name FROM users");
    REQUIRE(r.column_names == std::vector<std::string>{"age", "name"});
    REQUIRE(r.rows.size() == 5);
    CHECK(r.rows[0][0].i32  == 30);
    CHECK(r.rows[0][1].text == "alice");
}

TEST_CASE("WHERE on Int32 with > filters correctly") {
    TempFile tf;
    DiskManager dm(tf.path());
    BufferPool bp(8, &dm);
    Catalog cat = Catalog::create(&bp);
    cat.createTable("users", usersSchema());
    seedUsers(bp, *cat.getTable("users"));

    auto r = run(cat, bp, "SELECT name FROM users WHERE age > 25");
    CHECK(textCol(r, 0) ==
          std::vector<std::string>{"alice", "carol", "eve"});
}

TEST_CASE("WHERE on Int64 column with >=") {
    TempFile tf;
    DiskManager dm(tf.path());
    BufferPool bp(8, &dm);
    Catalog cat = Catalog::create(&bp);
    cat.createTable("users", usersSchema());
    seedUsers(bp, *cat.getTable("users"));

    auto r = run(cat, bp, "SELECT name FROM users WHERE salary >= 70000");
    CHECK(textCol(r, 0) ==
          std::vector<std::string>{"alice", "carol", "eve"});
}

TEST_CASE("WHERE on Text equality") {
    TempFile tf;
    DiskManager dm(tf.path());
    BufferPool bp(8, &dm);
    Catalog cat = Catalog::create(&bp);
    cat.createTable("users", usersSchema());
    seedUsers(bp, *cat.getTable("users"));

    auto r = run(cat, bp, "SELECT id FROM users WHERE name = 'carol'");
    REQUIRE(r.rows.size() == 1);
    CHECK(r.rows[0][0].i32 == 3);
}

TEST_CASE("WHERE on Bool equality") {
    TempFile tf;
    DiskManager dm(tf.path());
    BufferPool bp(8, &dm);
    Catalog cat = Catalog::create(&bp);
    cat.createTable("users", usersSchema());
    seedUsers(bp, *cat.getTable("users"));

    auto r = run(cat, bp, "SELECT name FROM users WHERE active = 0");
    CHECK(textCol(r, 0) == std::vector<std::string>{"carol", "eve"});
}

TEST_CASE("WHERE != excludes the matching row") {
    TempFile tf;
    DiskManager dm(tf.path());
    BufferPool bp(8, &dm);
    Catalog cat = Catalog::create(&bp);
    cat.createTable("users", usersSchema());
    seedUsers(bp, *cat.getTable("users"));

    auto r = run(cat, bp, "SELECT name FROM users WHERE name != 'alice'");
    CHECK(textCol(r, 0) ==
          std::vector<std::string>{"bob", "carol", "dave", "eve"});
}

TEST_CASE("WHERE that matches nothing yields zero rows but full header") {
    TempFile tf;
    DiskManager dm(tf.path());
    BufferPool bp(8, &dm);
    Catalog cat = Catalog::create(&bp);
    cat.createTable("users", usersSchema());
    seedUsers(bp, *cat.getTable("users"));

    auto r = run(cat, bp, "SELECT id, name FROM users WHERE age > 100");
    CHECK(r.rows.empty());
    CHECK(r.column_names == std::vector<std::string>{"id", "name"});
}

TEST_CASE("Scanning an empty table returns no rows but the right header") {
    TempFile tf;
    DiskManager dm(tf.path());
    BufferPool bp(8, &dm);
    Catalog cat = Catalog::create(&bp);
    cat.createTable("users", usersSchema());
    // Intentionally not seeded.

    auto r = run(cat, bp, "SELECT * FROM users");
    CHECK(r.rows.empty());
    CHECK(r.column_names.size() == 5);
}

TEST_CASE("Inner JOIN matches on equality and yields the cross-products") {
    TempFile tf;
    DiskManager dm(tf.path());
    BufferPool bp(8, &dm);
    Catalog cat = Catalog::create(&bp);
    cat.createTable("users", usersSchema());
    cat.createTable("posts", postsSchema());
    seedUsers(bp, *cat.getTable("users"));
    seedPosts(bp, *cat.getTable("posts"));

    auto r = run(cat, bp,
        "SELECT users.name, posts.title FROM users "
        "JOIN posts ON users.id = posts.user_id");

    // Outer = users (insertion order), inner = posts (insertion order). For
    // each user, posts whose user_id matches are emitted in posts' scan
    // order. dave has no matching posts, so he's absent from the result.
    REQUIRE(r.rows.size() == 5);
    CHECK(textCol(r, 0) ==
          std::vector<std::string>{"alice", "alice", "bob", "carol", "eve"});
    CHECK(textCol(r, 1) ==
          std::vector<std::string>{"a1", "a2", "b1", "c1", "e1"});
}

TEST_CASE("JOIN + WHERE filters across the joined row") {
    TempFile tf;
    DiskManager dm(tf.path());
    BufferPool bp(8, &dm);
    Catalog cat = Catalog::create(&bp);
    cat.createTable("users", usersSchema());
    cat.createTable("posts", postsSchema());
    seedUsers(bp, *cat.getTable("users"));
    seedPosts(bp, *cat.getTable("posts"));

    auto r = run(cat, bp,
        "SELECT users.name, posts.title FROM users "
        "JOIN posts ON users.id = posts.user_id WHERE users.age > 25");

    // bob (age 25) is filtered out; dave never had matching posts.
    CHECK(textCol(r, 0) ==
          std::vector<std::string>{"alice", "alice", "carol", "eve"});
    CHECK(textCol(r, 1) ==
          std::vector<std::string>{"a1", "a2", "c1", "e1"});
}

TEST_CASE("JOIN against an empty inner table yields zero rows") {
    TempFile tf;
    DiskManager dm(tf.path());
    BufferPool bp(8, &dm);
    Catalog cat = Catalog::create(&bp);
    cat.createTable("users", usersSchema());
    cat.createTable("posts", postsSchema());
    seedUsers(bp, *cat.getTable("users"));
    // posts left empty on purpose.

    auto r = run(cat, bp,
        "SELECT users.name, posts.title FROM users "
        "JOIN posts ON users.id = posts.user_id");
    CHECK(r.rows.empty());
}

TEST_CASE("Three-table chained JOIN") {
    TempFile tf;
    DiskManager dm(tf.path());
    BufferPool bp(8, &dm);
    Catalog cat = Catalog::create(&bp);

    Schema a_schema{{{"x", Type::Int32, false}}};
    Schema b_schema{{{"x", Type::Int32, false},
                     {"y", Type::Int32, false}}};
    Schema c_schema{{{"y", Type::Int32, false},
                     {"z", Type::Int32, false}}};
    cat.createTable("a", a_schema);
    cat.createTable("b", b_schema);
    cat.createTable("c", c_schema);

    {
        HeapFile hf(&bp, cat.getTable("a")->root_page);
        for (int32_t v : {1, 2, 3}) {
            const auto bytes = TupleCodec::encode(a_schema, {Value::Int32(v)});
            hf.insert(bytes.data(), bytes.size());
        }
    }
    {
        HeapFile hf(&bp, cat.getTable("b")->root_page);
        for (auto [x, y] : std::vector<std::pair<int32_t, int32_t>>{
                {1, 10}, {2, 20}, {3, 30}}) {
            const auto bytes = TupleCodec::encode(b_schema,
                {Value::Int32(x), Value::Int32(y)});
            hf.insert(bytes.data(), bytes.size());
        }
    }
    {
        // Only y=10 and y=30 have a c-side match — y=20 should drop out.
        HeapFile hf(&bp, cat.getTable("c")->root_page);
        for (auto [y, z] : std::vector<std::pair<int32_t, int32_t>>{
                {10, 100}, {30, 300}}) {
            const auto bytes = TupleCodec::encode(c_schema,
                {Value::Int32(y), Value::Int32(z)});
            hf.insert(bytes.data(), bytes.size());
        }
    }

    auto r = run(cat, bp,
        "SELECT a.x, c.z FROM a "
        "JOIN b ON a.x = b.x JOIN c ON b.y = c.y");

    REQUIRE(r.rows.size() == 2);
    CHECK(i32Col(r, 0) == std::vector<int32_t>{1, 3});
    CHECK(i32Col(r, 1) == std::vector<int32_t>{100, 300});
}

// ---- CREATE TABLE / INSERT through the executor ------------------------

TEST_CASE("CREATE TABLE adds the table to the catalog and SELECT sees it") {
    TempFile tf;
    DiskManager dm(tf.path());
    BufferPool bp(8, &dm);
    Catalog cat = Catalog::create(&bp);

    auto r = runStatement(cat, bp,
        "CREATE TABLE t (a INT NOT NULL, b TEXT)");

    CHECK(r.rows_affected == 0);
    CHECK(r.rows.empty());
    REQUIRE(cat.hasTable("t"));
    const auto* info = cat.getTable("t");
    REQUIRE(info != nullptr);
    REQUIRE(info->schema.columns.size() == 2);
    CHECK(info->schema.columns[0].name     == "a");
    CHECK(info->schema.columns[0].type     == Type::Int32);
    CHECK(info->schema.columns[0].nullable == false);
    CHECK(info->schema.columns[1].name     == "b");
    CHECK(info->schema.columns[1].type     == Type::Text);
    CHECK(info->schema.columns[1].nullable == true);

    // Empty SELECT works against the freshly-created table.
    auto sel = run(cat, bp, "SELECT * FROM t");
    CHECK(sel.rows.empty());
    CHECK(sel.column_names == std::vector<std::string>{"a", "b"});
}

TEST_CASE("INSERT writes rows visible to a follow-up SELECT") {
    TempFile tf;
    DiskManager dm(tf.path());
    BufferPool bp(8, &dm);
    Catalog cat = Catalog::create(&bp);

    runStatement(cat, bp,
        "CREATE TABLE users (id INT NOT NULL, name TEXT NOT NULL, age INT)");

    auto ins = runStatement(cat, bp,
        "INSERT INTO users VALUES "
        "(1, 'alice', 30), (2, 'bob', NULL), (3, 'carol', 40)");
    CHECK(ins.rows_affected == 3);
    CHECK(ins.rows.empty());

    auto sel = run(cat, bp, "SELECT id, name, age FROM users");
    REQUIRE(sel.rows.size() == 3);
    CHECK(sel.rows[0][0].i32  == 1);
    CHECK(sel.rows[0][1].text == "alice");
    CHECK(sel.rows[0][2].i32  == 30);
    CHECK(sel.rows[1][0].i32  == 2);
    CHECK(sel.rows[1][1].text == "bob");
    CHECK(sel.rows[1][2].is_null);
    CHECK(sel.rows[2][0].i32  == 3);
    CHECK(sel.rows[2][1].text == "carol");
}

TEST_CASE("INSERT with explicit column list reorders into schema order on disk") {
    TempFile tf;
    DiskManager dm(tf.path());
    BufferPool bp(8, &dm);
    Catalog cat = Catalog::create(&bp);

    runStatement(cat, bp,
        "CREATE TABLE t (id INT NOT NULL, name TEXT, score INT)");
    runStatement(cat, bp,
        "INSERT INTO t (score, id, name) VALUES (99, 1, 'alice')");

    auto sel = run(cat, bp, "SELECT * FROM t");
    REQUIRE(sel.rows.size() == 1);
    CHECK(sel.rows[0][0].i32  == 1);        // id
    CHECK(sel.rows[0][1].text == "alice");  // name
    CHECK(sel.rows[0][2].i32  == 99);       // score
}

TEST_CASE("INSERT followed by WHERE and JOIN through full SQL") {
    TempFile tf;
    DiskManager dm(tf.path());
    BufferPool bp(8, &dm);
    Catalog cat = Catalog::create(&bp);

    runStatement(cat, bp,
        "CREATE TABLE u (id INT NOT NULL, name TEXT NOT NULL)");
    runStatement(cat, bp,
        "CREATE TABLE p (uid INT NOT NULL, title TEXT NOT NULL)");
    runStatement(cat, bp,
        "INSERT INTO u VALUES (1, 'a'), (2, 'b'), (3, 'c')");
    runStatement(cat, bp,
        "INSERT INTO p VALUES (1, 'x'), (1, 'y'), (3, 'z')");

    auto r = run(cat, bp,
        "SELECT u.name, p.title FROM u JOIN p ON u.id = p.uid "
        "WHERE u.id != 2");
    CHECK(textCol(r, 0) == std::vector<std::string>{"a", "a", "c"});
    CHECK(textCol(r, 1) == std::vector<std::string>{"x", "y", "z"});
}

TEST_CASE("CREATE TABLE on an existing name throws at the executor") {
    TempFile tf;
    DiskManager dm(tf.path());
    BufferPool bp(8, &dm);
    Catalog cat = Catalog::create(&bp);

    runStatement(cat, bp, "CREATE TABLE t (a INT)");
    // The analyzer normally rejects this first. To exercise the
    // executor's own catalog-level guard, build a BoundCreateTable by
    // hand and invoke the per-statement primitive.
    BoundCreateTable bct;
    bct.name = "t";
    bct.schema.columns.push_back({"a", Type::Int32, true});
    Executor ex(&bp, &cat);
    CHECK_THROWS_AS(ex.execute(std::move(bct)), std::runtime_error);
}

TEST_CASE("Cold reopen sees rows inserted via SQL in a previous session") {
    TempFile tf;
    {
        DiskManager dm(tf.path());
        BufferPool bp(8, &dm);
        Catalog cat = Catalog::create(&bp);
        runStatement(cat, bp,
            "CREATE TABLE k (n INT NOT NULL, label TEXT NOT NULL)");
        runStatement(cat, bp,
            "INSERT INTO k VALUES (10, 'ten'), (20, 'twenty'), (30, 'thirty')");
        bp.flushAll();
    }

    DiskManager dm(tf.path());
    BufferPool bp(8, &dm);
    Catalog cat(&bp);

    auto r = run(cat, bp, "SELECT label FROM k WHERE n >= 20");
    CHECK(textCol(r, 0) == std::vector<std::string>{"twenty", "thirty"});
}

TEST_CASE("Cold reopen: data seeded in one session is visible to a fresh executor") {
    // Mirrors main.cpp's flow: write + flush, drop the in-memory state,
    // reopen the file, and run a query against a fresh Catalog and
    // Executor. Confirms the executor reads through the catalog rather
    // than depending on any in-process state from the seeding session.
    TempFile tf;
    {
        DiskManager dm(tf.path());
        BufferPool bp(8, &dm);
        Catalog cat = Catalog::create(&bp);
        cat.createTable("users", usersSchema());
        seedUsers(bp, *cat.getTable("users"));
        bp.flushAll();
    }

    DiskManager dm(tf.path());
    BufferPool bp(8, &dm);
    Catalog cat(&bp);  // bootstrap from pages 0/1

    auto r = run(cat, bp, "SELECT name FROM users WHERE age > 25");
    CHECK(textCol(r, 0) ==
          std::vector<std::string>{"alice", "carol", "eve"});
}
