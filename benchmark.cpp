#include "src/sql/parser.h"
#include "src/sql/analyzer.h"
#include "src/sql/catalog.h"
#include "src/sql/executor.h"
#include "src/sql/tuple.h"
#include "src/storage/buffer_pool.h"
#include "src/storage/disk_manager.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <string>
#include <utility>

// =============================================================================
// Insert/scan benchmark: load a large table that spans many pages, then time
// full scans and filtered scans through Parser → Analyzer → Executor.
//
// Usage: ./benchmark [row_count]   (default 50000)
// =============================================================================

namespace {

const std::string kDbPath = "/tmp/dbms_benchmark.db";

// Rows per INSERT statement. The parser builds the whole VALUES list in
// memory, so batching keeps statements reasonably sized while still
// exercising the multi-row INSERT path.
constexpr int kBatchSize = 500;

using Clock = std::chrono::steady_clock;

double msSince(Clock::time_point start) {
    auto end = Clock::now();
    return std::chrono::duration<double, std::milli>(end - start).count();
}

ExecResult run(Catalog& cat, BufferPool& bp, const std::string& sql) {
    Parser p(sql);
    Statement stmt = p.parse();
    Analyzer az(cat);
    BoundStatement bound = az.analyzeStatement(stmt);
    Executor ex(&bp, &cat);
    return ex.execute(std::move(bound));
}

}  // namespace

int main(int argc, char** argv) {
    int row_count = 50000;
    if (argc > 1) {
        row_count = std::atoi(argv[1]);
        if (row_count <= 0) {
            std::cerr << "row_count must be a positive integer\n";
            return 1;
        }
    }

    std::error_code ec;
    std::filesystem::remove(kDbPath, ec);

    DiskManager dm(kDbPath);
    BufferPool bp(64, &dm);
    Catalog cat = Catalog::create(&bp);

    std::cout << "rows: " << row_count << ", batch size: " << kBatchSize
              << ", page size: " << PAGE_SIZE << " bytes\n\n";

    run(cat, bp, "CREATE TABLE bench (id INT NOT NULL, "
                 "name TEXT NOT NULL, val INT NOT NULL)");

    // ---- Insert phase ----
    auto insert_start = Clock::now();
    int inserted = 0;
    while (inserted < row_count) {
        int batch_end = std::min(inserted + kBatchSize, row_count);
        std::string sql = "INSERT INTO bench VALUES ";
        for (int i = inserted; i < batch_end; ++i) {
            if (i > inserted) sql += ", ";
            sql += "(" + std::to_string(i) + ", 'row" + std::to_string(i) +
                   "', " + std::to_string(i % 1000) + ")";
        }
        run(cat, bp, sql);
        inserted = batch_end;
    }
    bp.flushAll();
    double insert_ms = msSince(insert_start);

    std::cout << "[insert] " << inserted << " rows in " << insert_ms
              << " ms (" << (inserted / insert_ms * 1000.0)
              << " rows/s)\n";
    std::cout << "[insert] file: " << dm.numPages() << " pages, "
              << std::filesystem::file_size(kDbPath) << " bytes\n\n";

    // ---- Full scan ----
    auto scan_start = Clock::now();
    ExecResult all = run(cat, bp, "SELECT * FROM bench");
    double scan_ms = msSince(scan_start);
    std::cout << "[scan ] SELECT * FROM bench -> " << all.rows.size()
              << " rows in " << scan_ms << " ms\n";

    // ---- Filtered scan ----
    auto filter_start = Clock::now();
    ExecResult filtered =
        run(cat, bp, "SELECT id, name FROM bench WHERE val = 42");
    double filter_ms = msSince(filter_start);
    std::cout << "[scan ] SELECT id, name FROM bench WHERE val = 42 -> "
              << filtered.rows.size() << " rows in " << filter_ms << " ms\n";

    // ---- Point-ish lookup near the end of the table ----
    auto point_start = Clock::now();
    ExecResult point = run(cat, bp,
        "SELECT * FROM bench WHERE id = " + std::to_string(row_count - 1));
    double point_ms = msSince(point_start);
    std::cout << "[scan ] SELECT * FROM bench WHERE id = " << (row_count - 1)
              << " -> " << point.rows.size() << " rows in " << point_ms
              << " ms\n";

    std::filesystem::remove(kDbPath, ec);
    return 0;
}
