#include "src/sql/analyzer.h"

#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>

Type resultTypeOf(const BoundExpr& e) {
    return std::visit(
        [](const auto& v) -> Type {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, BoundColumnRef>) return v.result_type;
            else if constexpr (std::is_same_v<T, BoundLiteral>) return v.result_type;
            else /* std::unique_ptr<BoundBinaryOp> */ return v->result_type;
        },
        e);
}

BoundColumnRef Analyzer::resolveColumn(const std::string& name,
                                       const Scope& scope) const {
    // "table.column" form: pin to one specific FROM table.
    const auto dot = name.find('.');
    if (dot != std::string::npos) {
        const std::string tbl = name.substr(0, dot);
        const std::string col = name.substr(dot + 1);
        for (size_t t = 0; t < scope.tables.size(); ++t) {
            if (scope.tables[t]->name == tbl) {
                const size_t c = scope.tables[t]->schema.indexOf(col);
                if (c == Schema::kNotFound) {
                    throw std::runtime_error("no such column '" + col +
                                             "' in table '" + tbl + "'");
                }
                return {t, c, scope.tables[t]->schema.columns[c].type};
            }
        }
        throw std::runtime_error("no such table '" + tbl + "' in FROM clause");
    }

    // Bare name: search every FROM table; reject if it appears in two.
    size_t found_t = 0;
    size_t found_c = 0;
    bool   found   = false;
    for (size_t t = 0; t < scope.tables.size(); ++t) {
        const size_t c = scope.tables[t]->schema.indexOf(name);
        if (c != Schema::kNotFound) {
            if (found) {
                throw std::runtime_error("ambiguous column '" + name +
                                         "': appears in multiple FROM tables");
            }
            found = true;
            found_t = t;
            found_c = c;
        }
    }
    if (!found) throw std::runtime_error("no such column '" + name + "'");
    return {found_t, found_c, scope.tables[found_t]->schema.columns[found_c].type};
}

BoundLiteral Analyzer::analyzeLiteral(const std::string& text,
                                      bool value_is_string,
                                      Type expected) const {
    if (value_is_string) {
        if (expected != Type::Text) {
            throw std::runtime_error(
                "string literal cannot be compared with a non-Text column");
        }
        return BoundLiteral{Value::Text(text), Type::Text};
    }

    // Numeric literal. Parse under the column's expected type.
    switch (expected) {
        case Type::Int32: {
            try {
                const long long v = std::stoll(text);
                if (v < std::numeric_limits<int32_t>::min() ||
                    v > std::numeric_limits<int32_t>::max()) {
                    throw std::runtime_error(
                        "integer literal out of Int32 range: " + text);
                }
                return BoundLiteral{Value::Int32(static_cast<int32_t>(v)),
                                    Type::Int32};
            } catch (const std::invalid_argument&) {
                throw std::runtime_error("invalid integer literal: " + text);
            } catch (const std::out_of_range&) {
                throw std::runtime_error("integer literal out of range: " + text);
            }
        }
        case Type::Int64: {
            try {
                const long long v = std::stoll(text);
                return BoundLiteral{Value::Int64(static_cast<int64_t>(v)),
                                    Type::Int64};
            } catch (const std::invalid_argument&) {
                throw std::runtime_error("invalid integer literal: " + text);
            } catch (const std::out_of_range&) {
                throw std::runtime_error("integer literal out of range: " + text);
            }
        }
        case Type::Bool: {
            // Parser has no TRUE/FALSE keyword; allow 0/1 against Bool columns.
            if (text == "0") return BoundLiteral{Value::Bool(false), Type::Bool};
            if (text == "1") return BoundLiteral{Value::Bool(true),  Type::Bool};
            throw std::runtime_error(
                "expected Bool literal (0 or 1), got: " + text);
        }
        case Type::Text:
            throw std::runtime_error(
                "Text column cannot be compared with a numeric literal");
    }
    throw std::runtime_error("analyzeLiteral: unknown expected type");
}

Type Analyzer::checkBinaryOp(Op op, Type lhs, Type rhs) const {
    if (lhs != rhs) {
        throw std::runtime_error("binary operator: type mismatch on operands");
    }
    switch (op) {
        case Op::Eq:
        case Op::Neq:
            // Equality is defined for every primitive type we have.
            return Type::Bool;
        case Op::Lt:
        case Op::Gt:
        case Op::Leq:
        case Op::Geq:
            // Ordering is numeric-only for now (no lexicographic Text yet).
            if (lhs != Type::Int32 && lhs != Type::Int64) {
                throw std::runtime_error(
                    "ordering operator requires numeric operands");
            }
            return Type::Bool;
    }
    throw std::runtime_error("checkBinaryOp: unknown operator");
}

BoundExpr Analyzer::analyzeCondition(const Condition& c,
                                     const Scope& scope) const {
    // Parser's WHERE shape is always <column> <op> <literal>. Resolve the
    // column first so we can use its type as the literal's expected type.
    BoundColumnRef col = resolveColumn(c.column, scope);
    BoundLiteral   lit = analyzeLiteral(c.value, c.value_is_string, col.result_type);
    const Type rt = checkBinaryOp(c.op, col.result_type, lit.result_type);

    auto bop = std::make_unique<BoundBinaryOp>();
    bop->op = c.op;
    bop->lhs = std::move(col);
    bop->rhs = std::move(lit);
    bop->result_type = rt;
    return BoundExpr{std::move(bop)};
}

BoundSelect Analyzer::analyze(const SelectQuery& q) const {
    BoundSelect out;

    // FROM clause + JOIN tables. Resolve every named table up front so the
    // ON clauses (and SELECT list, and WHERE) all see the full scope; this
    // matches SQL semantics for inner joins and keeps resolution uniform.
    const Catalog::TableInfo* info = cat_.getTable(q.table);
    if (info == nullptr) {
        throw std::runtime_error("no such table '" + q.table + "'");
    }
    out.from_tables.push_back(info);

    for (const auto& j : q.joins) {
        const Catalog::TableInfo* ji = cat_.getTable(j.table);
        if (ji == nullptr) {
            throw std::runtime_error("no such table '" + j.table + "'");
        }
        // Without aliases, the same table cannot appear twice — every
        // qualified reference would be ambiguous and bare references
        // would silently bind to the first copy.
        for (const auto* t : out.from_tables) {
            if (t->name == j.table) {
                throw std::runtime_error(
                    "table '" + j.table +
                    "' appears more than once in FROM/JOIN (aliases unsupported)");
            }
        }
        out.from_tables.push_back(ji);
    }

    Scope scope;
    scope.tables = out.from_tables;

    // ON clauses. Each join is `<col> = <col>`; both columns are resolved
    // against the full scope and must share a result type.
    for (const auto& j : q.joins) {
        BoundColumnRef l = resolveColumn(j.left,  scope);
        BoundColumnRef r = resolveColumn(j.right, scope);
        if (l.result_type != r.result_type) {
            throw std::runtime_error(
                "JOIN ON: type mismatch between '" + j.left +
                "' and '" + j.right + "'");
        }
        out.joins.push_back(BoundJoin{l, r});
    }

    // SELECT list.
    if (q.select_all) {
        out.select_all = true;
        for (size_t t = 0; t < out.from_tables.size(); ++t) {
            const auto& cols = out.from_tables[t]->schema.columns;
            for (size_t c = 0; c < cols.size(); ++c) {
                out.select_list.push_back(BoundColumnRef{t, c, cols[c].type});
            }
        }
    } else {
        for (const auto& name : q.columns) {
            out.select_list.push_back(resolveColumn(name, scope));
        }
    }

    // WHERE clause.
    if (q.where) {
        BoundExpr w = analyzeCondition(*q.where, scope);
        if (resultTypeOf(w) != Type::Bool) {
            throw std::runtime_error("WHERE clause must produce a Bool");
        }
        out.where = std::move(w);
    }

    return out;
}
