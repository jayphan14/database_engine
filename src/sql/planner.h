#pragma once

#include "src/sql/analyzer.h"
#include "src/sql/plan_node.h"
#include "src/storage/buffer_pool.h"

#include <memory>

// Translate a BoundSelect into a Volcano-style operator tree. No
// alternatives yet:
//
//   Filter(WHERE)?               <- only if bs.where is set
//     NestedLoopJoin(joins[k-1])
//       ...
//         NestedLoopJoin(joins[0])
//           SeqScan(from_tables[0])
//           SeqScan(from_tables[1])
//         SeqScan(from_tables[2])
//       ...
//       SeqScan(from_tables[k])
//
// Project is *not* a node — the Executor wraps the resulting tree to
// apply the SELECT list per row.
//
// `bs` is mutated: the WHERE expression is moved into the Filter node
// (left empty in `bs` afterwards). `bs.from_tables` and `bs.joins` are
// only read.
class Planner {
public:
    explicit Planner(BufferPool* bp) : bp_(bp) {}

    std::unique_ptr<PlanNode> plan(BoundSelect& bs) const;

private:
    BufferPool* bp_;
};
