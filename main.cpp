#include "src/sql/parser.h"
#include "src/sql/analyzer.h"
#include "src/sql/catalog.h"
#include "src/sql/executor.h"
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
#include <utility>
#include <variant>
#include <vector>

// =============================================================================
// End-to-end demo: seed a small users + posts dataset, cold-reopen the
// database, then run a handful of SQL strings through Parser → Analyzer →
// Executor and print the rows that come back.
// =============================================================================

namespace {

const std::string kDbPath = "/tmp/dbms_demo.db";

void seedUsers(BufferPool& bp, const Catalog::TableInfo& info) {
    const std::vector<std::tuple<int32_t, std::string, int32_t>> rows = {
        {1, "alice", 30},
        {2, "bob",   25},
        {3, "carol", 40},
        {4, "dave",  19},
        {5, "eve",   33},
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
}

void seedPosts(BufferPool& bp, const Catalog::TableInfo& info) {
    // (id, title, user_id) — user_id matches the users table above.
    const std::vector<std::tuple<int32_t, std::string, int32_t>> rows = {
        {100, "hello world",     1},
        {101, "second post",     1},
        {102, "carol's musings", 3},
        {103, "eve at midnight", 5},
        {104, "bob's silence",   2},
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

void printResult(const ExecResult& r) {
    // Column widths: max of header length and any value length, with a
    // small floor so single-char columns aren't crammed.
    std::vector<size_t> widths(r.column_names.size());
    for (size_t i = 0; i < r.column_names.size(); ++i) {
        widths[i] = std::max<size_t>(3, r.column_names[i].size());
    }
    for (const auto& row : r.rows) {
        for (size_t i = 0; i < row.size(); ++i) {
            widths[i] = std::max(widths[i], valueToString(row[i]).size());
        }
    }

    auto pad = [](const std::string& s, size_t w) {
        return s + std::string(w - s.size(), ' ');
    };

    std::cout << "  ";
    for (size_t i = 0; i < r.column_names.size(); ++i) {
        if (i) std::cout << " | ";
        std::cout << pad(r.column_names[i], widths[i]);
    }
    std::cout << "\n  ";
    for (size_t i = 0; i < widths.size(); ++i) {
        if (i) std::cout << "-+-";
        std::cout << std::string(widths[i], '-');
    }
    std::cout << "\n";

    for (const auto& row : r.rows) {
        std::cout << "  ";
        for (size_t i = 0; i < row.size(); ++i) {
            if (i) std::cout << " | ";
            std::cout << pad(valueToString(row[i]), widths[i]);
        }
        std::cout << "\n";
    }
    std::cout << "  (" << r.rows.size() << " row"
              << (r.rows.size() == 1 ? "" : "s") << ")\n";
}

void runQuery(const Catalog& cat, BufferPool& bp, const std::string& sql) {
    std::cout << "\nSQL: " << sql << "\n";
    try {
        Parser p(sql);
        Statement stmt = p.parse();
        // Demo only exercises SELECT today — DDL/DML are wired through
        // the catalog directly during seeding. Once the analyzer/executor
        // grow CREATE TABLE / INSERT support, this dispatch grows too.
        const auto* q = std::get_if<SelectQuery>(&stmt);
        if (q == nullptr) {
            std::cout << "  error: only SELECT is wired through the executor\n";
            return;
        }
        Analyzer az(cat);
        BoundSelect bs = az.analyze(*q);
        Executor ex(&bp);
        ExecResult r = ex.execute(std::move(bs));
        printResult(r);
    } catch (const std::exception& e) {
        std::cout << "  error: " << e.what() << "\n";
    }
}

void seedFreshDatabase() {
    std::error_code ec;
    std::filesystem::remove(kDbPath, ec);

    DiskManager dm(kDbPath);
    BufferPool bp(8, &dm);
    Catalog cat = Catalog::create(&bp);

    const Schema users_schema{{
        {"id",   Type::Int32, false},
        {"name", Type::Text,  false},
        {"age",  Type::Int32, false},
    }};
    const Schema posts_schema{{
        {"id",      Type::Int32, false},
        {"title",   Type::Text,  false},
        {"user_id", Type::Int32, false},
    }};
    cat.createTable("users", users_schema);
    cat.createTable("posts", posts_schema);

    seedUsers(bp, *cat.getTable("users"));
    seedPosts(bp, *cat.getTable("posts"));
    bp.flushAll();

    std::cout << "[seed] wrote users + posts to " << kDbPath
              << " (" << std::filesystem::file_size(kDbPath) << " bytes)\n";
}

}  // namespace

int main() {
    seedFreshDatabase();

    // Cold reopen — nothing is shared with the seeding phase except the file.
    DiskManager dm(kDbPath);
    BufferPool bp(8, &dm);
    Catalog cat(&bp);

    std::cout << "\n[query] reopened db; tables:";
    for (const auto& n : cat.tableNames()) std::cout << " " << n;
    std::cout << "\n";

    runQuery(cat, bp, "SELECT * FROM users");
    runQuery(cat, bp, "SELECT name, age FROM users WHERE age > 25");
    runQuery(cat, bp, "SELECT name FROM users WHERE name = 'alice'");
    runQuery(cat, bp,
        "SELECT users.name, posts.title "
        "FROM users JOIN posts ON users.id = posts.user_id");
    runQuery(cat, bp,
        "SELECT users.name, posts.title "
        "FROM users JOIN posts ON users.id = posts.user_id "
        "WHERE users.age > 25");

    std::error_code ec;
    std::filesystem::remove(kDbPath, ec);
    return 0;
}
