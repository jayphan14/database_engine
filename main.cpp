#include "src/parser.h"
#include "src/sql/catalog.h"
#include "src/sql/tuple.h"
#include "src/storage/buffer_pool.h"
#include "src/storage/disk_manager.h"
#include "src/storage/heap_file.h"

#include <cstdint>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <tuple>
#include <vector>

// =============================================================================
// Parser demo — parse a handful of SQL strings and print the resulting AST.
// =============================================================================

static void printQuery(const SelectQuery& q) {
    std::cout << "  columns: ";
    if (q.select_all) {
        std::cout << "*";
    } else {
        for (size_t i = 0; i < q.columns.size(); ++i) {
            if (i) std::cout << ", ";
            std::cout << q.columns[i];
        }
    }
    std::cout << "\n  table:   " << q.table << "\n";

    for (const auto& j : q.joins) {
        std::cout << "  join:    " << j.table
                  << " ON " << j.left << " = " << j.right << "\n";
    }

    if (q.where) {
        const auto& w = *q.where;
        std::cout << "  where:   " << w.column << " " << opToString(w.op) << " ";
        if (w.value_is_string) std::cout << "'" << w.value << "'";
        else                   std::cout << w.value;
        std::cout << "\n";
    } else {
        std::cout << "  where:   (none)\n";
    }
}

static void runParserDemo() {
    std::cout << "=== Parser demo ==========================================\n\n";
    const std::vector<std::string> queries = {
        "SELECT id, name FROM users WHERE age > 18",
        "SELECT * FROM products",
        "SELECT name FROM users,posts WHERE name = 'alice'",
        "select email, id from accounts where status != 'banned'",
        "SELECT u.id, u.name, p.title FROM users JOIN posts ON u.id = p.user_id",
        "SELECT * FROM a JOIN b ON a.x = b.x JOIN c ON b.y = c.y WHERE c.z > 0",
        "SELECT FROM users",
    };
    for (const auto& sql : queries) {
        std::cout << "SQL: " << sql << "\n";
        try {
            Parser p(sql);
            SelectQuery q = p.parse();
            printQuery(q);
        } catch (const std::exception& e) {
            std::cout << "  error:   " << e.what() << "\n";
        }
        std::cout << "\n";
    }
}

// =============================================================================
// Storage demo — exercise the full storage + catalog stack end-to-end:
//
//   1. open a brand-new database file
//   2. create the catalog (allocates __tables and __columns at pages 0/1)
//   3. createTable("users", schema)
//   4. insert a few rows via TupleCodec → HeapFile
//   5. flush, drop the in-memory state, reopen
//   6. open the catalog with no extra information, look up "users",
//      open its heap file, scan and print every row
// =============================================================================

namespace {

void seedUsers(BufferPool& bp, const Catalog::TableInfo& info) {
    const std::vector<std::tuple<int32_t, std::string, int32_t>> rows = {
        {1, "alice",   30},
        {2, "bob",     25},
        {3, "carol",   40},
        {4, "dave",    19},
        {5, "eve",     33},
    };
    HeapFile hf(&bp, info.root_page);
    for (const auto& [id, name, age] : rows) {
        const auto bytes = TupleCodec::encode(info.schema, {
            Value::Int32(id),
            Value::Text(name),
            Value::Int32(age),
        });
        hf.insert(bytes.data(), bytes.size());
    }
    std::cout << "  inserted " << rows.size() << " rows into 'users'\n";
}

void scanUsers(BufferPool& bp, const Catalog::TableInfo& info) {
    HeapFile hf(&bp, info.root_page);
    for (const auto& [rid, bytes] : hf) {
        const auto vals = TupleCodec::decode(info.schema, bytes.data(), bytes.size());
        std::cout << "  rid=(" << rid.page_id << "," << rid.slot_id << ")"
                  << "  id=" << vals[0].i32
                  << "  name=" << vals[1].text
                  << "  age=" << vals[2].i32 << "\n";
    }
}

}  // namespace

static void runStorageDemo() {
    std::cout << "=== Storage demo =========================================\n\n";
    const std::string path = "/tmp/dbms_demo.db";

    // Start from a clean slate so the demo is reproducible.
    std::error_code ec;
    std::filesystem::remove(path, ec);

    // ----- Phase 1: create + populate ------------------------------------
    {
        DiskManager dm(path);
        BufferPool bp(8, &dm);
        Catalog cat = Catalog::create(&bp);

        const Schema users_schema{{
            {"id",   Type::Int32, false},
            {"name", Type::Text,  false},
            {"age",  Type::Int32, true},
        }};
        cat.createTable("users", users_schema);

        std::cout << "[phase 1] created catalog + table 'users'\n";
        std::cout << "  __tables  is at page " << Catalog::TABLES_ROOT  << "\n";
        std::cout << "  __columns is at page " << Catalog::COLUMNS_ROOT << "\n";
        std::cout << "  users     heap is at page "
                  << cat.getTable("users")->root_page << "\n";

        seedUsers(bp, *cat.getTable("users"));
        bp.flushAll();
        std::cout << "  flushed; file size = "
                  << std::filesystem::file_size(path) << " bytes\n\n";
    }

    // ----- Phase 2: cold reopen + scan -----------------------------------
    DiskManager dm(path);
    BufferPool bp(8, &dm);
    Catalog cat(&bp);  // bootstrap from pages 0 + 1

    std::cout << "[phase 2] reopened database; tables in catalog:";
    for (const auto& n : cat.tableNames()) std::cout << " " << n;
    std::cout << "\n";

    const auto* info = cat.getTable("users");
    std::cout << "  scanning 'users':\n";
    scanUsers(bp, *info);
    std::cout << "\n";

    // Tidy up so successive runs always start clean.
    std::filesystem::remove(path, ec);
}

int main() {
    // runParserDemo();
    runStorageDemo();
    return 0;
}
