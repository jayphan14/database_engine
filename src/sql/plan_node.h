#pragma once

#include "src/sql/analyzer.h"   // BoundExpr, Op
#include "src/sql/tuple.h"      // Value, Type

#include <iosfwd>
#include <optional>
#include <vector>

// One materialized intermediate row, partitioned by FROM-table index:
// row[t][c] is the value of column c of from_tables[t]. SeqScan
// populates exactly one slot; NestedLoopJoin overlays its children's
// (disjoint) slots into a combined row. Slots that no operator in the
// current subtree has populated stay empty.
using ExecRow = std::vector<std::vector<Value>>;

// Pull-based (Volcano) operator interface. Lifecycle:
//
//   open() exactly once before the first next();
//   next() returns nullopt when exhausted;
//   close() exactly once when the consumer is done.
//
// Operators own their per-call state and their child operators
// (transferred in via std::unique_ptr at construction).
class PlanNode {
public:
    virtual ~PlanNode() = default;

    virtual void open() = 0;
    virtual std::optional<ExecRow> next() = 0;
    virtual void close() = 0;

    // Indented one-line-per-node EXPLAIN-style description. Each level
    // is two spaces of leading indent; children print with indent + 1.
    virtual void describe(std::ostream& os, int indent) const = 0;
};

// Evaluate a bound expression against a wide row. Used by Filter (for
// WHERE) and by Executor (for the final projection).
Value evalExpr(const BoundExpr& e, const ExecRow& row);

// Apply a binary operator over two same-type Values. Equality uses
// Value's ==; ordering is Int32/Int64 only — the analyzer rejects
// ordering on other types, so anything else here is a bug upstream.
Value evalBinaryOp(Op op, const Value& l, const Value& r);
