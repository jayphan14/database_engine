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
// End-to-end demo: run a CREATE TABLE, an INSERT, and a SELECT through
// Parser → Analyzer → Executor against a fresh database to exercise the
// full statement cycle.
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

}  // namespace

int main() {
    std::error_code ec;
    std::filesystem::remove(kDbPath, ec);

    DiskManager dm(kDbPath);
    BufferPool bp(8, &dm);
    // Catalog::create allocates the system-table bootstrap pages
    // (__tables at page 0, __columns at page 1) — user tables are
    // built via SQL below.
    Catalog cat = Catalog::create(&bp);

    runStatement(cat, bp,
        "CREATE TABLE users (id INT NOT NULL, "
        "name TEXT NOT NULL, age INT NOT NULL)");
    runStatement(cat, bp,
        "INSERT INTO users VALUES "
        "(1, 'alice', 30), (2, 'bob', 25), (3, 'carol', 40)");
    runStatement(cat, bp, "SELECT * FROM users");

    bp.flushAll();

    std::filesystem::remove(kDbPath, ec);
    return 0;
}
