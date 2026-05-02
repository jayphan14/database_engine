# database_engine

A small disk-backed database engine in C++17. The project is being built
bottom-up — page-level storage first, then a tuple/catalog layer, then the
SQL front end. There is no executor yet; the front end stops at a typed,
name-resolved AST.

## What's implemented

The pieces below are wired end-to-end and exercised by `main.cpp`.

### Storage stack (`src/storage/`)

A four-layer stack on a single file of fixed-size 4 KiB pages:

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

- An executor / query plan. Nothing runs a `BoundSelect` against the
  storage stack — the demo seeds and scans heap files directly.
- DML (`INSERT` / `UPDATE` / `DELETE`) at the SQL surface.
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

`main.cpp` toggles between two demos:

- **Parser demo** — parses a handful of example queries (including joins
  and a deliberately malformed one) and prints each AST.
- **Storage demo** — opens a fresh database file, creates a `users`
  table through the catalog, inserts five rows via `TupleCodec` +
  `HeapFile`, flushes, then *cold-reopens* the file and scans every row
  back out using only the catalog bootstrap. This is the end-to-end
  smoke test for the storage + catalog stack.

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
an end-to-end integration test), the tuple codec, the catalog, and the
analyzer. Pass doctest flags by invoking the binary directly, e.g.:

```sh
build/run_tests --help
build/run_tests --test-case="SELECT *"
```

## Clean

```sh
make clean
```

Removes `build/` and the `dbms` binary.
