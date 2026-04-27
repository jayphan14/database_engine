#include "src/sql/catalog.h"

#include <algorithm>
#include <cstdint>
#include <map>
#include <stdexcept>
#include <utility>

Schema Catalog::tablesSchema() {
    return Schema{{
        {"table_id",      Type::Int32, false},
        {"name",          Type::Text,  false},
        {"first_page_id", Type::Int64, false},
    }};
}

Schema Catalog::columnsSchema() {
    return Schema{{
        {"table_id", Type::Int32, false},
        {"position", Type::Int32, false},
        {"name",     Type::Text,  false},
        {"type",     Type::Int32, false},
        {"nullable", Type::Bool,  false},
    }};
}

Catalog::Catalog(BufferPool* bp)
    : bp_(bp),
      tables_hf_(bp, TABLES_ROOT),
      columns_hf_(bp, COLUMNS_ROOT),
      next_table_id_(0) {
    loadFromDisk();
}

Catalog Catalog::create(BufferPool* bp) {
    // Allocating __tables must yield page 0; __columns must yield page 1.
    // Anything else means the database already had data, in which case the
    // bootstrap convention is broken and we'd silently mis-read on reopen.
    HeapFile tables_hf = HeapFile::create(bp);
    if (tables_hf.firstPageId() != TABLES_ROOT) {
        throw std::runtime_error(
            "Catalog::create: __tables did not land at page 0; "
            "is the database already initialized?");
    }
    HeapFile columns_hf = HeapFile::create(bp);
    if (columns_hf.firstPageId() != COLUMNS_ROOT) {
        throw std::runtime_error(
            "Catalog::create: __columns did not land at page 1; "
            "is the database already initialized?");
    }
    return Catalog(bp);
}

void Catalog::loadFromDisk() {
    const Schema tables_s  = tablesSchema();
    const Schema columns_s = columnsSchema();

    // Pass 1: pull every row out of __tables. Schemas are filled in in pass 2.
    std::vector<TableInfo> infos;
    for (auto it = tables_hf_.begin(); it != tables_hf_.end(); ++it) {
        const auto& bytes = it->second;
        auto vals = TupleCodec::decode(tables_s, bytes.data(), bytes.size());
        TableInfo info;
        info.table_id  = vals[0].i32;
        info.name      = vals[1].text;
        info.root_page = static_cast<PageId>(vals[2].i64);
        infos.push_back(std::move(info));
    }

    // Pass 2: collect column rows from __columns, group by table_id, sort by
    // position. Sorting by position lets us reconstruct the user's column
    // order even if the catalog was edited out of order across crashes.
    struct ColRow {
        int32_t  position;
        std::string name;
        Type type;
        bool nullable;
    };
    std::map<int32_t, std::vector<ColRow>> by_table;
    for (auto it = columns_hf_.begin(); it != columns_hf_.end(); ++it) {
        const auto& bytes = it->second;
        auto vals = TupleCodec::decode(columns_s, bytes.data(), bytes.size());
        const int32_t tid = vals[0].i32;
        ColRow c{vals[1].i32, vals[2].text, typeFromCode(vals[3].i32), vals[4].b};
        by_table[tid].push_back(std::move(c));
    }

    // Pass 3: stitch each table's columns into its Schema, install in cache,
    // and track the next free table_id.
    int32_t max_id = -1;
    for (auto& info : infos) {
        auto cols_it = by_table.find(info.table_id);
        if (cols_it != by_table.end()) {
            auto& cols = cols_it->second;
            std::sort(cols.begin(), cols.end(),
                      [](const ColRow& a, const ColRow& b) {
                          return a.position < b.position;
                      });
            info.schema.columns.reserve(cols.size());
            for (auto& c : cols) {
                info.schema.columns.push_back({std::move(c.name), c.type, c.nullable});
            }
        }
        max_id = std::max(max_id, info.table_id);
        tables_.emplace(info.name, std::move(info));
    }
    next_table_id_ = max_id + 1;
}

void Catalog::createTable(const std::string& name, Schema schema) {
    if (hasTable(name)) {
        throw std::runtime_error("Catalog: table '" + name + "' already exists");
    }

    // Allocate the user heap file first. If we crash before recording in
    // the catalog, the worst case is one orphan page; the alternative
    // (record-first, allocate-after) leaves dangling rows pointing at
    // nothing.
    HeapFile new_hf = HeapFile::create(bp_);
    const PageId root = new_hf.firstPageId();
    const int32_t table_id = next_table_id_++;

    // Append to __tables.
    {
        const auto bytes = TupleCodec::encode(tablesSchema(), {
            Value::Int32(table_id),
            Value::Text(name),
            Value::Int64(static_cast<int64_t>(root)),
        });
        tables_hf_.insert(bytes.data(), bytes.size());
    }

    // Append one row per column to __columns. Position is the column's
    // index in the user's schema and is what we sort by on reload.
    const Schema cs = columnsSchema();
    for (size_t i = 0; i < schema.columns.size(); ++i) {
        const auto& col = schema.columns[i];
        const auto bytes = TupleCodec::encode(cs, {
            Value::Int32(table_id),
            Value::Int32(static_cast<int32_t>(i)),
            Value::Text(col.name),
            Value::Int32(typeToCode(col.type)),
            Value::Bool(col.nullable),
        });
        columns_hf_.insert(bytes.data(), bytes.size());
    }

    tables_.emplace(name, TableInfo{table_id, name, std::move(schema), root});
}

const Catalog::TableInfo* Catalog::getTable(const std::string& name) const {
    auto it = tables_.find(name);
    return it == tables_.end() ? nullptr : &it->second;
}

std::vector<std::string> Catalog::tableNames() const {
    std::vector<std::string> out;
    out.reserve(tables_.size());
    for (const auto& kv : tables_) out.push_back(kv.first);
    return out;
}
