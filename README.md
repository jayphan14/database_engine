# database_engine

A small disk-backed database engine in C++17. The project is being built
bottom-up: page-level storage first, then a tuple/catalog layer, then the
SQL front end (parser → analyzer), then a planner that lowers the bound
AST into a tree of Volcano-style operators, and finally an executor that
drives that tree to produce rows.

```
SQL string
  → Parser     → SelectQuery        (string-based AST)
  → Analyzer   → BoundSelect        (numeric indices + types)
  → Planner    → PlanNode tree      (SeqScan / NestedLoopJoin / Filter)
  → Executor   → ExecResult         (rows of Values)
```

## What's implemented

The pieces below are wired end-to-end and exercised by `main.cpp`.

### Storage stack (`src/storage/`, plus `tuple` / `catalog` from `src/sql/`)

A layered stack over a single OS file of fixed-size 4 KiB pages. Each
layer adds *meaning* over the bytes below; only `DiskManager` ever
touches the file.

```
   ┌───────────────────────────────────────────────────┐
   │ Catalog          name → (schema, root_page)       │
   │ TupleCodec       Values ↔ bytes (schema-aware)    │
   ├───────────────────────────────────────────────────┤
   │ HeapFile         chain of slotted pages           │
   │ SlottedPage      one page's header + slots + rows │
   ├───────────────────────────────────────────────────┤
   │ BufferPool       RAM cache: LRU, pin/dirty bits,  │
   │                  PageGuard (RAII)                 │
   ├───────────────────────────────────────────────────┤
   │ DiskManager      page N ↔ byte offset N × 4096    │
   └────────────────────────┬──────────────────────────┘
                            ▼
                  ┌─────────────────────┐
                  │   one OS file       │
                  │   all durable state │
                  └─────────────────────┘
```

The four core storage layers, bottom-up:

1. **`DiskManager`** — read/write/allocate raw pages by `PageId`. Page N
   lives at byte offset `N * PAGE_SIZE`. Files only grow.
2. **`BufferPool`** — fixed-size frame array caching pages from the
   `DiskManager`. LRU eviction over unpinned frames; `PageGuard` (RAII)
   handles pin/unpin and dirty marking automatically.
3. **`SlottedPage`** — byte-level page format. Header + slot directory
   growing down, packed tuple bytes growing up. Slot IDs are stable
   across compaction; `remove` leaves a tombstone that a later insert
   may reuse.
4. **`HeapFile`** — an unordered collection of tuples spread across a
   chain of slotted pages linked by each page's `next_page_id`. Supports
   `insert`, `remove`, point lookup by `RID`, and forward iteration.

After seeding, the demo's database file looks like this on disk:

```
/tmp/dbms_demo.db   (16 KiB total = 4 pages)

  ┌──────────┬──────────┬──────────┬──────────┐
  │  page 0  │  page 1  │  page 2  │  page 3  │
  │ __tables │ __columns│  users   │  posts   │
  └──────────┴──────────┴──────────┴──────────┘
   0–4095     4096–8191  8192–12287 12288–16383     (byte offsets)

Each page is a 4 KiB SlottedPage:

  ┌─────────┬─────────────────┬──────────┬───────────────┐
  │ header  │ slot[0] slot[1] │  ← free  │  … tuples …   │
  │ ~16 B   │ (grows down)    │          │  (grow up)    │
  └─────────┴─────────────────┴──────────┴───────────────┘
```

`__tables` and `__columns` are the catalog's two system tables (rows
of `(table_id, name, root_page)` and `(table_id, position, name, type,
nullable)`). They live at hard-coded page IDs so cold-open requires no
manifest — bootstrap is "open the heap files at pages 0 and 1, walk
them, rebuild every user table's `TableInfo` in memory."

### SQL layer (`src/sql/`)

- **`tuple.{h,cpp}`** — `Type`, `Value`, `Schema`, and `TupleCodec` for
  encoding/decoding a row to/from bytes. `Int32` / `Int64` / `Bool` are
  fixed width; `Text` is length-prefixed.
- **`catalog.{h,cpp}`** — persistent table catalog stored as two
  bootstrap heap files at hard-coded pages: `__tables` at page 0 and
  `__columns` at page 1. On startup the catalog is reconstructed into
  an in-memory cache; `createTable` updates both the disk system
  tables and the cache.
- **`analyzer.{h,cpp}`** — walks a parsed `SelectQuery` against a
  `Catalog`, resolves every name to numeric indices, and type-checks
  every operator. Produces a `BoundSelect` (the parallel of the parser
  AST, but with strings replaced by `(table_index, column_index)` pairs
  and every node carrying a `result_type`). Throws on name-resolution
  failure or type mismatch.
- **`plan_node.{h,cpp}`** — the `PlanNode` interface (Volcano-style:
  `open() / next() / close() / describe()`) and the wide-row
  `ExecRow = vector<vector<Value>>` shape every operator emits. Plus
  free `evalExpr` / `evalBinaryOp` for use by Filter and projection.
- **`operators.{h,cpp}`** — three concrete `PlanNode`s:
  - `SeqScan` — walks one heap file, decodes each tuple, populates
    its assigned slot in the wide row.
  - `NestedLoopJoin` — materializes the right child on `open()`,
    nested-loops over (left × right), overlays the two children's
    disjoint slots, emits combined rows where the ON predicate holds.
  - `Filter` — pulls from its child until the predicate is true.
- **`planner.{h,cpp}`** — `Planner::plan(BoundSelect&)`: a 1:1 lowering
  to a left-deep operator tree (outer `SeqScan`, then a stack of
  `NestedLoopJoin`s, then an optional `Filter` on top). No
  alternatives yet — when there are (hash join, index scan, predicate
  pushdown), this is the seam where an optimizer plugs in.
- **`executor.{h,cpp}`** — thin coordinator. Builds the plan, drives
  `root->open() / next() / close()`, applies the SELECT list to each
  wide row to produce a flat-row `ExecResult { column_names,
  column_types, rows }`. `execute(BoundSelect bs)` consumes its
  argument because the planner moves the WHERE expression out.

### SQL pipeline — worked example

A query like `SELECT name FROM users WHERE age > 18` flows through four
stages, each producing a different intermediate representation:

```
SQL string
  "SELECT name FROM users WHERE age > 18"
            │
            ▼   Parser  (tokenize + recursive-descent)
            │
  SelectQuery (string-based AST)
    table   = "users"
    columns = ["name"]
    where   = { column: "age", op: Gt, value: "18" }
            │
            ▼   Analyzer  (resolve names against Catalog, type-check)
            │
  BoundSelect (numeric indices + types)
    from_tables = [ users_info ]
    select_list = [ ColumnRef{0, 1, Text} ]            // "name"
    where = BinaryOp{
              Gt,
              ColumnRef{0, 2, Int32},                  // "age"
              Literal{18, Int32}
            }
            │
            ▼   Planner  (1:1 lowering to operator tree)
            │
  PlanNode tree
    Filter(age > 18)
      └── SeqScan(users)
            │
            ▼   Executor  (open / next / close, project)
            │
            ▼
  ExecResult
    column_names = ["name"]
    column_types = [Text]
    rows         = [ ["alice"], ["carol"], ["eve"] ]
```

Every layer's job in one phrase: **Parser** turns characters into a
tree of strings; **Analyzer** turns strings into integers (against the
`Catalog`); **Planner** turns the tree into operators; **Executor**
drives the operators and projects the SELECT list.

### Parser (`src/parser.{h,cpp}`)

Hand-written recursive-descent parser for a single `SELECT` statement.
Supported grammar:

```
SELECT (* | <col-list>)
FROM <table>
{JOIN <table> ON <col> = <col>}
[WHERE <col> <op> <literal>]
```

Columns may be qualified (`users.id`). Comparison ops: `= != < > <= >=`.
Joins are inner joins on a single equality between two columns.

### What the analyzer accepts today

- `SELECT *` and explicit column lists, qualified or bare.
- One or more `JOIN ... ON <col> = <col>` clauses; ON columns may
  reference any table in the FROM/JOIN list and must share a type.
- A `WHERE <col> <op> <literal>` clause; the literal is parsed under the
  column's expected type, with range checking for `Int32`.
- Bare-column ambiguity, unknown table/column, and type-mismatch errors
  are all reported as `std::runtime_error`.

Aliases are not yet supported, so the same physical table cannot appear
twice in one query.

### What's not built yet

- DML (`INSERT` / `UPDATE` / `DELETE`) at the SQL surface — rows are
  inserted today via `HeapFile::insert` + `TupleCodec::encode`, not SQL.
- `ORDER BY`, `LIMIT`, aggregates, expressions in the SELECT list, and
  table aliases.
- Alternative operators (`HashJoin`, `IndexScan`, ...) and a real
  optimizer that picks between them. The plan tree is now a data
  structure, so these slot in as new `PlanNode` subclasses plus
  rewrite passes — but neither exists yet. The current planner does a
  fixed 1:1 lowering.
- `EXPLAIN` at the SQL surface. Operators already have a `describe()`
  method, so it's a thin addition once we want to wire it up.
- Indexes, transactions, recovery, concurrency control.

## Requirements

- `g++` with C++17 support (or `clang++` — adjust `CXX` in the `Makefile`)
- `make`

## Build and run

Build the `dbms` binary:

```sh
make dbms
```

Run the demo (`main.cpp`):

```sh
./dbms
```

`main.cpp` is one end-to-end flow that exercises every layer:

1. open a fresh database file, create the catalog, declare a `users` and
   a `posts` table;
2. seed both tables (5 rows each) via `TupleCodec` + `HeapFile`, flush;
3. *cold-reopen* the file with a brand-new `BufferPool` and `Catalog`;
4. run a handful of SQL strings (including filters and a join)
   through Parser → Analyzer → Planner → Executor and print each
   result as a padded table.

Or do both build + run in one step:

```sh
make run
```

## Run tests locally

Tests use [doctest](https://github.com/doctest/doctest) (vendored at
`tests/vendor/doctest.h`, no install needed):

```sh
make test
```

This builds and runs `build/run_tests`, which covers the parser, every
storage layer (disk manager, buffer pool, slotted page, heap file, plus
an end-to-end integration test), the tuple codec, the catalog, the
analyzer, the executor (end-to-end SQL → ExecResult cases including a
cold-reopen), and the operators directly (`SeqScan` / `Filter` /
`NestedLoopJoin` state machines, empty-input edge cases, and the
`describe()` EXPLAIN output). Pass doctest flags by invoking the binary
directly, e.g.:

```sh
build/run_tests --help
build/run_tests --test-case="SELECT *"
```

## Clean

```sh
make clean
```

Removes `build/` and the `dbms` binary.
