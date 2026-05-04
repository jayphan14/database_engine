#pragma once

#include "src/sql/analyzer.h"          // BoundExpr, BoundJoin
#include "src/sql/catalog.h"           // Catalog::TableInfo
#include "src/sql/plan_node.h"
#include "src/storage/buffer_pool.h"
#include "src/storage/heap_file.h"

#include <cstddef>
#include <iosfwd>
#include <memory>
#include <optional>
#include <vector>

// Walk every row of one table's heap file, decode it via TupleCodec,
// and emit it into the wide-row slot the analyzer assigned this table
// (table_index). All other slots stay empty until a join populates
// them. Holds no state across queries — open() rebuilds the iterator,
// close() tears it down.
class SeqScan : public PlanNode {
public:
    SeqScan(BufferPool* bp,
            const Catalog::TableInfo* info,
            std::size_t table_index,
            std::size_t total_tables);

    void open() override;
    std::optional<ExecRow> next() override;
    void close() override;
    void describe(std::ostream& os, int indent) const override;

private:
    BufferPool*               bp_;
    const Catalog::TableInfo* info_;
    std::size_t               table_index_;
    std::size_t               total_tables_;

    // Lazily constructed in open(); HeapFile's iterator references the
    // heap file via its BufferPool, so the file outlives the iterator.
    // A default-constructed Iterator is the end iterator, so the
    // "never opened" and "exhausted" states are indistinguishable —
    // either way next() returns nullopt.
    std::unique_ptr<HeapFile>  hf_;
    HeapFile::Iterator         it_;
};

// Outer = left, inner = right. Materializes the right side once on
// open(); for each (left, right) pair, overlays the two wide rows
// (slot-by-slot) and emits the combined row iff the bound ON predicate
// holds. The planner guarantees left and right populate disjoint slot
// sets, so overlay is well-defined.
class NestedLoopJoin : public PlanNode {
public:
    NestedLoopJoin(std::unique_ptr<PlanNode> left,
                   std::unique_ptr<PlanNode> right,
                   BoundJoin on);

    void open() override;
    std::optional<ExecRow> next() override;
    void close() override;
    void describe(std::ostream& os, int indent) const override;

private:
    std::unique_ptr<PlanNode> left_;
    std::unique_ptr<PlanNode> right_;
    BoundJoin                 on_;

    std::vector<ExecRow>      right_buffered_;
    std::optional<ExecRow>    cur_left_;
    std::size_t               right_pos_ = 0;
};

// Drop rows from `child` for which `pred` doesn't evaluate to true.
// Predicate must produce Bool; the analyzer enforces this for WHERE.
class Filter : public PlanNode {
public:
    Filter(std::unique_ptr<PlanNode> child, BoundExpr pred);

    void open() override;
    std::optional<ExecRow> next() override;
    void close() override;
    void describe(std::ostream& os, int indent) const override;

private:
    std::unique_ptr<PlanNode> child_;
    BoundExpr                 pred_;
};
