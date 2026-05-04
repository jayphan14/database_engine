#include "src/sql/operators.h"

#include "src/sql/tuple.h"

#include <ostream>
#include <utility>

namespace {

void indentLine(std::ostream& os, int indent) {
    for (int i = 0; i < indent; ++i) os << "  ";
}

}  // namespace

// =====================================================================
// SeqScan
// =====================================================================

SeqScan::SeqScan(BufferPool* bp,
                 const Catalog::TableInfo* info,
                 std::size_t table_index,
                 std::size_t total_tables)
    : bp_(bp),
      info_(info),
      table_index_(table_index),
      total_tables_(total_tables) {}

void SeqScan::open() {
    hf_ = std::make_unique<HeapFile>(bp_, info_->root_page);
    it_ = hf_->begin();
}

std::optional<ExecRow> SeqScan::next() {
    if (!hf_ || it_ == hf_->end()) return std::nullopt;

    const auto& [rid, bytes] = *it_;
    (void)rid;
    auto vals = TupleCodec::decode(info_->schema, bytes.data(), bytes.size());

    ExecRow row(total_tables_);
    row[table_index_] = std::move(vals);

    ++it_;
    return row;
}

void SeqScan::close() {
    it_ = HeapFile::Iterator{};
    hf_.reset();
}

void SeqScan::describe(std::ostream& os, int indent) const {
    indentLine(os, indent);
    os << "SeqScan(" << info_->name << ")\n";
}

// =====================================================================
// NestedLoopJoin
// =====================================================================

NestedLoopJoin::NestedLoopJoin(std::unique_ptr<PlanNode> left,
                               std::unique_ptr<PlanNode> right,
                               BoundJoin on)
    : left_(std::move(left)),
      right_(std::move(right)),
      on_(on) {}

void NestedLoopJoin::open() {
    left_->open();

    // Materialize the right side eagerly so the inner loop is a plain
    // vector walk. We're done with the right operator after this.
    right_->open();
    while (auto r = right_->next()) {
        right_buffered_.push_back(std::move(*r));
    }
    right_->close();

    cur_left_  = left_->next();
    right_pos_ = 0;
}

std::optional<ExecRow> NestedLoopJoin::next() {
    while (cur_left_) {
        while (right_pos_ < right_buffered_.size()) {
            const ExecRow& r = right_buffered_[right_pos_++];

            // Overlay r's populated slots onto a copy of cur_left_.
            // The planner guarantees disjointness of populated slots
            // between left and right children of any given join, so
            // the overlay never silently overwrites real data.
            ExecRow combined = *cur_left_;
            for (std::size_t i = 0; i < r.size(); ++i) {
                if (!r[i].empty()) combined[i] = r[i];
            }

            const Value& lv = combined[on_.left.table_index ][on_.left.column_index ];
            const Value& rv = combined[on_.right.table_index][on_.right.column_index];
            if (lv == rv) return combined;
        }
        cur_left_  = left_->next();
        right_pos_ = 0;
    }
    return std::nullopt;
}

void NestedLoopJoin::close() {
    left_->close();
    cur_left_.reset();
    right_buffered_.clear();
    right_pos_ = 0;
}

void NestedLoopJoin::describe(std::ostream& os, int indent) const {
    indentLine(os, indent);
    os << "NestedLoopJoin(["
       << on_.left.table_index  << "][" << on_.left.column_index  << "] = ["
       << on_.right.table_index << "][" << on_.right.column_index << "])\n";
    left_->describe(os, indent + 1);
    right_->describe(os, indent + 1);
}

// =====================================================================
// Filter
// =====================================================================

Filter::Filter(std::unique_ptr<PlanNode> child, BoundExpr pred)
    : child_(std::move(child)),
      pred_(std::move(pred)) {}

void Filter::open() {
    child_->open();
}

std::optional<ExecRow> Filter::next() {
    while (auto row = child_->next()) {
        const Value v = evalExpr(pred_, *row);
        if (v.b) return row;
    }
    return std::nullopt;
}

void Filter::close() {
    child_->close();
}

void Filter::describe(std::ostream& os, int indent) const {
    indentLine(os, indent);
    os << "Filter\n";
    child_->describe(os, indent + 1);
}
