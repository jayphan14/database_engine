#pragma once

#include "src/sql/analyzer.h"
#include "src/sql/catalog.h"
#include "src/sql/tuple.h"
#include "src/storage/buffer_pool.h"

#include <cstddef>
#include <string>
#include <vector>

// Materialized result of running a bound statement.
//
// SELECT populates `column_names`, `column_types`, and `rows`; for
// SELECT, `rows_affected` is left at 0 (the row count is already in
// `rows.size()`).
//
// CREATE TABLE / INSERT leave the column/row vectors empty and use
// `rows_affected` as a Postgres-style command tag — 0 for CREATE,
// the inserted row count for INSERT.
struct ExecResult {
    std::vector<std::string> column_names;
    std::vector<Type>        column_types;
    std::vector<std::vector<Value>> rows;
    std::size_t rows_affected = 0;
};

// Thin coordinator over the analyzer's bound IR.
//
// SELECT path: builds a Volcano-style plan via Planner, drives the
// root operator's open/next/close, and applies the SELECT list to each
// row to produce a flat-row ExecResult.
//
// CREATE TABLE: forwards the bound schema to the catalog.
// INSERT: opens a HeapFile at the bound table's root page and inserts
// one encoded tuple per BoundInsert row.
//
// All execute() overloads consume their argument (the planner moves
// the WHERE expression out of a BoundSelect; CREATE moves the schema
// into the catalog). Pass rvalues at the call site (std::move).
//
// `bp` and `cat` are non-owning. The catalog is mutable because
// CREATE TABLE updates it; mutating-through-pointer keeps the
// execute() methods const-correct.
class Executor {
public:
    Executor(BufferPool* bp, Catalog* cat) : bp_(bp), cat_(cat) {}

    // Top-level entry: dispatches on the analyzer's variant alternative.
    ExecResult execute(BoundStatement bs) const;

    // Per-statement primitives. Tests and call sites that already know
    // the statement kind use these directly.
    ExecResult execute(BoundSelect      bs) const;
    ExecResult execute(BoundCreateTable bs) const;
    ExecResult execute(BoundInsert      bs) const;

private:
    BufferPool* bp_;
    Catalog*    cat_;
};
