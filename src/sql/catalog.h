#pragma once

#include "src/sql/tuple.h"
#include "src/storage/buffer_pool.h"
#include "src/storage/disk_manager.h"
#include "src/storage/heap_file.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

// Persistent table catalog, stored as two ordinary heap files:
//
//   __tables (table_id, name, first_page_id)
//   __columns(table_id, position, name, type, nullable)
//
// One row per user table in __tables, one row per column in __columns.
// These system tables live at hard-coded page ids — page 0 is __tables,
// page 1 is __columns. That hard-coding is the bootstrap: opening a
// database is "open those two heap files at known offsets and walk them."
//
// On startup the catalog is fully reconstructed in memory and serves as
// a read-through cache. Mutations (createTable) update both system
// tables on disk and the in-memory cache.
//
// __tables and __columns are not themselves listed in __tables — they
// live below the catalog. This avoids the chicken-and-egg of needing
// the catalog to read the catalog.
//
// Single-process. If you have two Catalog instances open on the same
// database file they will both load on construction but won't see each
// other's edits.
class Catalog {
public:
    // Hard-coded bootstrap pages. They are the first two pages allocated
    // on a brand-new database file by Catalog::create.
    static constexpr PageId TABLES_ROOT  = 0;
    static constexpr PageId COLUMNS_ROOT = 1;

    struct TableInfo {
        int32_t     table_id;
        std::string name;
        Schema      schema;
        PageId      root_page;
    };

    // Open an existing catalog. Reads __tables and __columns at the
    // bootstrap page ids and rebuilds the in-memory cache.
    explicit Catalog(BufferPool* bp);

    // Allocate the bootstrap pages on a brand-new database and return a
    // ready-to-use Catalog. Throws if the disk already has pages
    // allocated, since that would leave __tables and __columns at the
    // wrong page ids.
    static Catalog create(BufferPool* bp);

    // Allocate a heap file for `name`, append one row to __tables and
    // schema.columns.size() rows to __columns, update the cache.
    // Throws if `name` already exists.
    void createTable(const std::string& name, Schema schema);

    bool hasTable(const std::string& name) const {
        return tables_.count(name) != 0;
    }

    // Returns nullptr when absent. The pointer is stable across later
    // createTable calls (unordered_map insertions don't invalidate
    // references) and remains valid for this Catalog's lifetime.
    const TableInfo* getTable(const std::string& name) const;

    std::vector<std::string> tableNames() const;

private:
    BufferPool* bp_;
    HeapFile tables_hf_;
    HeapFile columns_hf_;
    std::unordered_map<std::string, TableInfo> tables_;
    int32_t next_table_id_;

    // Hard-coded schemas of the two system tables.
    static Schema tablesSchema();
    static Schema columnsSchema();

    // Walk __tables + __columns and populate `tables_` and `next_table_id_`.
    void loadFromDisk();
};
