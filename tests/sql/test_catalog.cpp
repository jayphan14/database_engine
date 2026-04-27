#include "tests/vendor/doctest.h"

#include "src/sql/catalog.h"
#include "src/sql/tuple.h"
#include "src/storage/buffer_pool.h"
#include "src/storage/disk_manager.h"
#include "src/storage/heap_file.h"
#include "tests/test_util.h"

#include <algorithm>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

Schema makePersonSchema() {
    return Schema{{
        {"id",   Type::Int32, false},
        {"name", Type::Text,  false},
        {"age",  Type::Int32, true},
    }};
}

Schema makeOrderSchema() {
    return Schema{{
        {"id",          Type::Int64, false},
        {"customer",    Type::Text,  false},
        {"total_cents", Type::Int32, false},
        {"shipped",     Type::Bool,  false},
    }};
}

}  // namespace

TEST_CASE("Catalog::create allocates pages 0 and 1 as the bootstrap heap files") {
    TempFile tf;
    DiskManager dm(tf.path());
    BufferPool bp(4, &dm);
    Catalog cat = Catalog::create(&bp);

    CHECK(Catalog::TABLES_ROOT  == 0);
    CHECK(Catalog::COLUMNS_ROOT == 1);
    // Both system tables have been allocated.
    CHECK(dm.numPages() == 2);
}

TEST_CASE("Catalog::create on a non-empty disk throws") {
    TempFile tf;
    DiskManager dm(tf.path());
    BufferPool bp(4, &dm);

    // Pretend something else allocated page 0 already.
    dm.allocatePage();
    CHECK_THROWS_AS(Catalog::create(&bp), std::runtime_error);
}

TEST_CASE("a fresh catalog reports no user tables") {
    TempFile tf;
    DiskManager dm(tf.path());
    BufferPool bp(4, &dm);
    Catalog cat = Catalog::create(&bp);

    CHECK_FALSE(cat.hasTable("anything"));
    CHECK(cat.getTable("anything") == nullptr);
    CHECK(cat.tableNames().empty());
}

TEST_CASE("createTable registers a table that hasTable / getTable can find") {
    TempFile tf;
    DiskManager dm(tf.path());
    BufferPool bp(4, &dm);
    Catalog cat = Catalog::create(&bp);

    cat.createTable("people", makePersonSchema());

    CHECK(cat.hasTable("people"));
    const auto* info = cat.getTable("people");
    REQUIRE(info != nullptr);
    CHECK(info->name == "people");
    CHECK(info->table_id == 0);
    REQUIRE(info->schema.columns.size() == 3);
    CHECK(info->schema.columns[0].name == "id");
    CHECK(info->schema.columns[0].type == Type::Int32);
    CHECK_FALSE(info->schema.columns[0].nullable);
    CHECK(info->schema.columns[1].type == Type::Text);
    CHECK(info->schema.columns[2].nullable);
    // Brand-new table sits past the bootstrap pages.
    CHECK(info->root_page > Catalog::COLUMNS_ROOT);
}

TEST_CASE("createTable with a duplicate name throws") {
    TempFile tf;
    DiskManager dm(tf.path());
    BufferPool bp(4, &dm);
    Catalog cat = Catalog::create(&bp);

    cat.createTable("people", makePersonSchema());
    CHECK_THROWS_AS(cat.createTable("people", makePersonSchema()),
                    std::runtime_error);
}

TEST_CASE("table_ids are assigned monotonically in creation order") {
    TempFile tf;
    DiskManager dm(tf.path());
    BufferPool bp(4, &dm);
    Catalog cat = Catalog::create(&bp);

    cat.createTable("a", makePersonSchema());
    cat.createTable("b", makeOrderSchema());
    cat.createTable("c", makePersonSchema());

    CHECK(cat.getTable("a")->table_id == 0);
    CHECK(cat.getTable("b")->table_id == 1);
    CHECK(cat.getTable("c")->table_id == 2);
}

TEST_CASE("multiple tables coexist with distinct heap files") {
    TempFile tf;
    DiskManager dm(tf.path());
    BufferPool bp(4, &dm);
    Catalog cat = Catalog::create(&bp);

    cat.createTable("people", makePersonSchema());
    cat.createTable("orders", makeOrderSchema());

    auto names = cat.tableNames();
    REQUIRE(names.size() == 2);
    std::sort(names.begin(), names.end());
    CHECK(names[0] == "orders");
    CHECK(names[1] == "people");

    const auto* p = cat.getTable("people");
    const auto* o = cat.getTable("orders");
    REQUIRE(p);
    REQUIRE(o);
    CHECK(p->root_page != o->root_page);
    CHECK(p->root_page != Catalog::TABLES_ROOT);
    CHECK(p->root_page != Catalog::COLUMNS_ROOT);
    CHECK(o->root_page != Catalog::TABLES_ROOT);
    CHECK(o->root_page != Catalog::COLUMNS_ROOT);
}

TEST_CASE("schema with all four column types round-trips through the catalog") {
    TempFile tf;
    DiskManager dm(tf.path());
    BufferPool bp(4, &dm);
    Catalog cat = Catalog::create(&bp);

    Schema mixed{{
        {"a", Type::Int32, true},
        {"b", Type::Int64, false},
        {"c", Type::Bool,  true},
        {"d", Type::Text,  false},
    }};
    cat.createTable("mixed", mixed);

    const auto* info = cat.getTable("mixed");
    REQUIRE(info);
    REQUIRE(info->schema.columns.size() == 4);
    CHECK(info->schema.columns[0].type == Type::Int32);
    CHECK(info->schema.columns[0].nullable);
    CHECK(info->schema.columns[1].type == Type::Int64);
    CHECK_FALSE(info->schema.columns[1].nullable);
    CHECK(info->schema.columns[2].type == Type::Bool);
    CHECK(info->schema.columns[2].nullable);
    CHECK(info->schema.columns[3].type == Type::Text);
    CHECK_FALSE(info->schema.columns[3].nullable);
}

TEST_CASE("catalog persists across DiskManager / BufferPool restart") {
    TempFile tf;
    {
        DiskManager dm(tf.path());
        BufferPool bp(4, &dm);
        Catalog cat = Catalog::create(&bp);
        cat.createTable("people", makePersonSchema());
        cat.createTable("orders", makeOrderSchema());
        bp.flushAll();
    }

    // Cold reopen: only `tf.path()` survives across the boundary.
    DiskManager dm(tf.path());
    BufferPool bp(4, &dm);
    Catalog cat(&bp);

    CHECK(cat.hasTable("people"));
    CHECK(cat.hasTable("orders"));

    const auto* p = cat.getTable("people");
    REQUIRE(p);
    CHECK(p->table_id == 0);
    REQUIRE(p->schema.columns.size() == 3);
    CHECK(p->schema.columns[1].name == "name");
    CHECK(p->schema.columns[1].type == Type::Text);
    CHECK(p->schema.columns[2].nullable);

    const auto* o = cat.getTable("orders");
    REQUIRE(o);
    CHECK(o->table_id == 1);
    REQUIRE(o->schema.columns.size() == 4);
    CHECK(o->schema.columns[3].type == Type::Bool);
}

TEST_CASE("table_ids do not collide with new tables created after restart") {
    TempFile tf;
    {
        DiskManager dm(tf.path());
        BufferPool bp(4, &dm);
        Catalog cat = Catalog::create(&bp);
        cat.createTable("a", makePersonSchema());
        cat.createTable("b", makePersonSchema());
        bp.flushAll();
    }

    DiskManager dm(tf.path());
    BufferPool bp(4, &dm);
    Catalog cat(&bp);
    cat.createTable("c", makePersonSchema());

    CHECK(cat.getTable("a")->table_id == 0);
    CHECK(cat.getTable("b")->table_id == 1);
    CHECK(cat.getTable("c")->table_id == 2);
}

TEST_CASE("a registered table's heap file is independently usable") {
    TempFile tf;
    DiskManager dm(tf.path());
    BufferPool bp(4, &dm);
    Catalog cat = Catalog::create(&bp);
    cat.createTable("people", makePersonSchema());

    const auto* info = cat.getTable("people");
    REQUIRE(info);

    HeapFile people(&bp, info->root_page);

    auto bytes = TupleCodec::encode(info->schema, {
        Value::Int32(1),
        Value::Text("alice"),
        Value::Int32(30),
    });
    RID r = people.insert(bytes.data(), bytes.size());

    std::string out;
    REQUIRE(people.get(r, &out));
    auto vals = TupleCodec::decode(info->schema, out.data(), out.size());
    CHECK(vals[0].i32 == 1);
    CHECK(vals[1].text == "alice");
    CHECK(vals[2].i32 == 30);
}

TEST_CASE("end-to-end: catalog + 100 rows, restart, full scan") {
    TempFile tf;
    {
        DiskManager dm(tf.path());
        BufferPool bp(4, &dm);
        Catalog cat = Catalog::create(&bp);
        cat.createTable("people", makePersonSchema());

        const auto* info = cat.getTable("people");
        REQUIRE(info);
        HeapFile people(&bp, info->root_page);
        for (int i = 0; i < 100; ++i) {
            auto bytes = TupleCodec::encode(info->schema, {
                Value::Int32(i),
                Value::Text("name_" + std::to_string(i)),
                Value::Int32(20 + (i % 50)),
            });
            people.insert(bytes.data(), bytes.size());
        }
        bp.flushAll();
    }

    DiskManager dm(tf.path());
    BufferPool bp(4, &dm);
    Catalog cat(&bp);

    const auto* info = cat.getTable("people");
    REQUIRE(info);
    HeapFile people(&bp, info->root_page);

    int count = 0;
    int sum_ids = 0;
    for (const auto& [rid, bytes] : people) {
        (void)rid;
        auto vals = TupleCodec::decode(info->schema, bytes.data(), bytes.size());
        REQUIRE(vals.size() == 3);
        sum_ids += vals[0].i32;
        ++count;
    }
    CHECK(count == 100);
    CHECK(sum_ids == 100 * 99 / 2);
}

TEST_CASE("getTable pointer remains valid after subsequent createTable calls") {
    TempFile tf;
    DiskManager dm(tf.path());
    BufferPool bp(4, &dm);
    Catalog cat = Catalog::create(&bp);

    cat.createTable("first", makePersonSchema());
    const auto* first = cat.getTable("first");
    REQUIRE(first);
    const PageId first_root = first->root_page;

    cat.createTable("second", makeOrderSchema());
    cat.createTable("third",  makePersonSchema());

    CHECK(first->name == "first");
    CHECK(first->root_page == first_root);
}

TEST_CASE("__tables and __columns hold the expected number of rows") {
    TempFile tf;
    DiskManager dm(tf.path());
    BufferPool bp(4, &dm);
    Catalog cat = Catalog::create(&bp);

    cat.createTable("people", makePersonSchema());   // 3 columns
    cat.createTable("orders", makeOrderSchema());    // 4 columns

    // Reach into the system tables directly to confirm row counts.
    HeapFile tables_hf(&bp, Catalog::TABLES_ROOT);
    HeapFile columns_hf(&bp, Catalog::COLUMNS_ROOT);

    int n_tables = 0;
    for (auto it = tables_hf.begin(); it != tables_hf.end(); ++it) ++n_tables;
    CHECK(n_tables == 2);

    int n_columns = 0;
    for (auto it = columns_hf.begin(); it != columns_hf.end(); ++it) ++n_columns;
    CHECK(n_columns == 3 + 4);
}
