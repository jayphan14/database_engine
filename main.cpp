#include "src/sql/parser.h"
#include "src/sql/analyzer.h"
#include "src/sql/catalog.h"
#include "src/sql/executor.h"
#include "src/sql/tuple.h"
#include "src/storage/buffer_pool.h"
#include "src/storage/disk_manager.h"

#include <cstdint>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

// =============================================================================
// End-to-end demo: seed a small users + posts dataset via SQL, cold-reopen the
// database, then run a handful of queries through Parser → Analyzer →
// Executor and print the rows that come back.
// =============================================================================

namespace {

const std::string kDbPath = "/tmp/dbms_demo.db";

void printSelectResult(const ExecResult& r) {
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

// Run one statement end-to-end. Picks a render based on the parsed
// statement kind: SELECT prints a padded table, CREATE TABLE / INSERT
// print a Postgres-style command tag.
void runStatement(Catalog& cat, BufferPool& bp, const std::string& sql) {
    std::cout << "\nSQL: " << sql << "\n";
    try {
        Parser p(sql);
        Statement stmt = p.parse();
        Analyzer az(cat);
        BoundStatement bound = az.analyzeStatement(stmt);
        Executor ex(&bp, &cat);
        ExecResult r = ex.execute(std::move(bound));

        if (std::holds_alternative<SelectQuery>(stmt)) {
            printSelectResult(r);
        } else if (std::holds_alternative<CreateTableStmt>(stmt)) {
            std::cout << "  CREATE TABLE\n";
        } else {
            std::cout << "  INSERT " << r.rows_affected << "\n";
        }
    } catch (const std::exception& e) {
        std::cout << "  error: " << e.what() << "\n";
    }
}

void seedFreshDatabase() {
    std::error_code ec;
    std::filesystem::remove(kDbPath, ec);

    DiskManager dm(kDbPath);
    BufferPool bp(8, &dm);
    // Catalog::create still allocates the system-table bootstrap pages
    // (__tables at page 0, __columns at page 1) — only user tables are
    // built via SQL.
    Catalog cat = Catalog::create(&bp);

    runStatement(cat, bp,
        "CREATE TABLE users (id INT NOT NULL, "
        "name TEXT NOT NULL, age INT NOT NULL)");
    runStatement(cat, bp,
        "CREATE TABLE posts (id INT NOT NULL, "
        "title TEXT NOT NULL, user_id INT NOT NULL)");

    runStatement(cat, bp,
        "INSERT INTO users VALUES "
        "(1, 'alice', 30), (2, 'bob', 25), (3, 'carol', 40), "
        "(4, 'dave', 19), (5, 'eve', 33)");
    // The lexer has no string-escape handling, so titles avoid apostrophes
    // — see grammar.md's "not yet supported" list.
    runStatement(cat, bp,
        "INSERT INTO posts VALUES "
        "(100, 'hello world', 1), (101, 'second post', 1), "
        "(102, 'musings of carol', 3), (103, 'eve at midnight', 5), "
        "(104, 'silence from bob', 2)");

    bp.flushAll();
    std::cout << "\n[seed] wrote users + posts to " << kDbPath
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

    runStatement(cat, bp, "SELECT * FROM users");
    runStatement(cat, bp, "SELECT name, age FROM users WHERE age > 25");
    runStatement(cat, bp, "SELECT name FROM users WHERE name = 'alice'");
    runStatement(cat, bp,
        "SELECT users.name, posts.title "
        "FROM users JOIN posts ON users.id = posts.user_id");
    runStatement(cat, bp,
        "SELECT users.name, posts.title "
        "FROM users JOIN posts ON users.id = posts.user_id "
        "WHERE users.age > 25");

    std::error_code ec;
    std::filesystem::remove(kDbPath, ec);
    return 0;
}
