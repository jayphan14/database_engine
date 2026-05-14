#pragma once

#include "src/sql/parser.h"
#include "src/sql/catalog.h"
#include "src/sql/tuple.h"

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

// Bound (resolved) parallel of the parser's AST. Strings — column names,
// table names — have all been replaced by numeric indices into a schema,
// so the executor can look up values by integer indexing rather than by
// re-resolving names per row.

struct BoundColumnRef {
    size_t table_index;   // index into BoundSelect::from_tables
    size_t column_index;  // index into that table's schema.columns
    Type   result_type;
};

struct BoundLiteral {
    Value value;
    Type  result_type;    // == value.type
};

struct BoundBinaryOp;     // forward; needed because BoundExpr can recurse

// std::unique_ptr<BoundBinaryOp> works as a variant alternative even when
// BoundBinaryOp is incomplete here, because unique_ptr only needs the
// pointee's size to be known at construction/destruction (not declaration).
using BoundExpr = std::variant<BoundColumnRef,
                                BoundLiteral,
                                std::unique_ptr<BoundBinaryOp>>;

struct BoundBinaryOp {
    Op       op;
    BoundExpr lhs;
    BoundExpr rhs;
    Type     result_type;
};

// Result type accessor that handles every variant alternative uniformly.
Type resultTypeOf(const BoundExpr& e);

// Resolved `JOIN <table> ON <left> = <right>`. Both columns are bound
// against the full FROM scope, so either side may refer to any joined
// table (not only the two flanking this clause). The newly-joined table
// itself sits at from_tables[i + 1] for the i-th element of `joins`.
struct BoundJoin {
    BoundColumnRef left;
    BoundColumnRef right;
};

struct BoundSelect {
    // Resolved FROM tables, in declaration order: index 0 is the FROM
    // table, indices 1..n are JOIN tables in the order they appeared.
    // A BoundColumnRef::table_index indexes into this vector.
    std::vector<const Catalog::TableInfo*> from_tables;

    // One per JOIN clause, in source order. joins[i] introduces
    // from_tables[i + 1].
    std::vector<BoundJoin> joins;

    // One BoundExpr per output column. For SELECT *, populated with one
    // BoundColumnRef per (table, column) across every from_tables entry.
    std::vector<BoundExpr> select_list;

    // Bound WHERE predicate, if the query had one. result_type is Bool.
    std::optional<BoundExpr> where;

    // Tracks whether the parsed query was SELECT *. Mainly informational
    // — select_list is already expanded.
    bool select_all = false;
};

// Bound CREATE TABLE: surface type names (e.g. "INT", "TEXT") have been
// mapped to Type, and column names checked unique. The catalog is *not*
// touched at bind time — the executor performs the actual creation, so
// the analyzer also pre-checks that the table name is free.
struct BoundCreateTable {
    std::string name;
    Schema      schema;
};

// Bound INSERT: every literal cell has been parsed into a typed Value
// (NULL becomes Value::Null(col.type)). `rows` is in schema column
// order — if the user wrote a column list, the analyzer has already
// reordered each row and filled unspecified columns with NULL. Each
// row has exactly `table->schema.columns.size()` values.
struct BoundInsert {
    const Catalog::TableInfo*       table;
    std::vector<std::vector<Value>> rows;
};

// A bound (analyzed) top-level SQL statement. Mirrors `Statement` in
// parser.h: each parser variant alternative maps to one bound variant
// alternative.
using BoundStatement = std::variant<BoundSelect,
                                    BoundCreateTable,
                                    BoundInsert>;

// Walks a parsed SelectQuery against a Catalog, resolving every name and
// type-checking every operator. Throws std::runtime_error on any name
// resolution failure or type mismatch. Pure: no I/O of its own beyond the
// catalog lookups it performs.
class Analyzer {
public:
    explicit Analyzer(const Catalog& cat) : cat_(cat) {}

    // Top-level entry: dispatches on the parser's variant alternative.
    BoundStatement analyzeStatement(const Statement& s) const;

    // Per-statement entry points. Overloaded so analyzeStatement can
    // dispatch with a single visit, and so call sites that already know
    // the statement kind (e.g. existing tests) can stay terse.
    BoundSelect      analyze(const SelectQuery& q) const;
    BoundCreateTable analyze(const CreateTableStmt& s) const;
    BoundInsert      analyze(const InsertStmt& s) const;

private:
    // Set of tables visible to expression resolution. For now, just the
    // FROM clause's tables in order.
    struct Scope {
        std::vector<const Catalog::TableInfo*> tables;
    };

    BoundColumnRef resolveColumn(const std::string& name,
                                 const Scope& scope) const;
    BoundLiteral   analyzeLiteral(const std::string& text,
                                  bool value_is_string,
                                  Type expected) const;
    BoundExpr      analyzeCondition(const Condition& c,
                                    const Scope& scope) const;
    Type           checkBinaryOp(Op op, Type lhs, Type rhs) const;

    // Convert one INSERT cell against the column it lands in. Handles
    // NULL specially (rejected when the column is non-nullable); for
    // non-NULL it delegates to analyzeLiteral. Type-name resolution
    // for CREATE TABLE lives in tuple.h's free `typeFromName`.
    Value analyzeInsertCell(const InsertLiteral& lit,
                            const Column& col) const;

    const Catalog& cat_;
};
