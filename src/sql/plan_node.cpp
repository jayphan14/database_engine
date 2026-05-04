#include "src/sql/plan_node.h"

#include <stdexcept>
#include <type_traits>
#include <variant>

namespace {

bool valueLess(const Value& a, const Value& b) {
    if (a.type == Type::Int32) return a.i32 < b.i32;
    if (a.type == Type::Int64) return a.i64 < b.i64;
    throw std::runtime_error("evalBinaryOp: ordering on non-numeric type");
}

}  // namespace

Value evalBinaryOp(Op op, const Value& l, const Value& r) {
    switch (op) {
        case Op::Eq:  return Value::Bool(l == r);
        case Op::Neq: return Value::Bool(l != r);
        case Op::Lt:  return Value::Bool(valueLess(l, r));
        case Op::Gt:  return Value::Bool(valueLess(r, l));
        case Op::Leq: return Value::Bool(!valueLess(r, l));
        case Op::Geq: return Value::Bool(!valueLess(l, r));
    }
    throw std::runtime_error("evalBinaryOp: unknown operator");
}

Value evalExpr(const BoundExpr& e, const ExecRow& row) {
    return std::visit(
        [&](const auto& v) -> Value {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, BoundColumnRef>) {
                return row[v.table_index][v.column_index];
            } else if constexpr (std::is_same_v<T, BoundLiteral>) {
                return v.value;
            } else /* std::unique_ptr<BoundBinaryOp> */ {
                const Value l = evalExpr(v->lhs, row);
                const Value r = evalExpr(v->rhs, row);
                return evalBinaryOp(v->op, l, r);
            }
        },
        e);
}
