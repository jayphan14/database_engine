#include "src/sql/executor.h"

#include "src/sql/plan_node.h"
#include "src/sql/planner.h"

#include <utility>
#include <variant>

ExecResult Executor::execute(BoundSelect bs) const {
    Planner planner(bp_);
    auto root = planner.plan(bs);

    // Output column metadata, derived from the SELECT list. Pulled
    // before draining so a query that returns zero rows still produces
    // the right header.
    ExecResult out;
    out.column_names.reserve(bs.select_list.size());
    out.column_types.reserve(bs.select_list.size());
    for (const auto& expr : bs.select_list) {
        if (const auto* col = std::get_if<BoundColumnRef>(&expr)) {
            const auto& schema = bs.from_tables[col->table_index]->schema;
            out.column_names.push_back(schema.columns[col->column_index].name);
        } else {
            // Current grammar can't reach this branch (SELECT only
            // accepts column refs), but the type system permits it.
            out.column_names.push_back("?");
        }
        out.column_types.push_back(resultTypeOf(expr));
    }

    // Drive the root operator and project each emitted wide row.
    root->open();
    while (auto row = root->next()) {
        std::vector<Value> projected;
        projected.reserve(bs.select_list.size());
        for (const auto& expr : bs.select_list) {
            projected.push_back(evalExpr(expr, *row));
        }
        out.rows.push_back(std::move(projected));
    }
    root->close();

    return out;
}
