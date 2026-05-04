#include "src/sql/planner.h"

#include "src/sql/operators.h"

#include <utility>

std::unique_ptr<PlanNode> Planner::plan(BoundSelect& bs) const {
    const std::size_t n = bs.from_tables.size();

    // Outer scan.
    std::unique_ptr<PlanNode> root =
        std::make_unique<SeqScan>(bp_, bs.from_tables[0], 0, n);

    // Stack joins left-deep. Join `i` introduces from_tables[i + 1] as
    // its right child.
    for (std::size_t i = 0; i < bs.joins.size(); ++i) {
        auto right = std::make_unique<SeqScan>(
            bp_, bs.from_tables[i + 1], i + 1, n);
        root = std::make_unique<NestedLoopJoin>(
            std::move(root), std::move(right), bs.joins[i]);
    }

    // WHERE, if any. Move the predicate so the operator owns it.
    if (bs.where) {
        root = std::make_unique<Filter>(
            std::move(root), std::move(*bs.where));
        bs.where.reset();
    }

    return root;
}
