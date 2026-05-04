#pragma once

#include "src/sql/analyzer.h"
#include "src/sql/tuple.h"
#include "src/storage/buffer_pool.h"

#include <string>
#include <vector>

// Materialized result of running a BoundSelect: column metadata plus
// one vector<Value> per output row, in scan order. Every row has
// exactly column_names.size() values.
struct ExecResult {
    std::vector<std::string> column_names;
    std::vector<Type>        column_types;
    std::vector<std::vector<Value>> rows;
};

// Thin coordinator: builds a Volcano-style plan via Planner, drives
// the root operator's open/next/close, and applies the SELECT list to
// each row to produce a flat-row ExecResult.
//
// `bs` is consumed — the planner moves the WHERE expression out of it.
// Pass an rvalue at the call site (std::move).
class Executor {
public:
    explicit Executor(BufferPool* bp) : bp_(bp) {}

    ExecResult execute(BoundSelect bs) const;

private:
    BufferPool* bp_;
};
