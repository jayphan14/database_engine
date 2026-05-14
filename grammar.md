# Supported SQL grammar

This is the surface grammar accepted by `Parser` (`src/sql/parser.{h,cpp}`).
Notation: `<x>` is a non-terminal, `[x]` is optional, `{x}` is zero-or-more,
`UPPERCASE` keywords are case-insensitive, single-quoted strings are literal
text. Whitespace between tokens is insignificant.

The parser is a single-statement parser: each call to `Parser::parse()`
consumes exactly one statement and rejects trailing tokens.

## Top-level

```
<statement>     ::= <select-stmt>
                  | <create-table-stmt>
                  | <insert-stmt>
```

`Parser::parse()` returns a `Statement` variant whose alternative reflects
the production matched.

## SELECT

```
<select-stmt>   ::= SELECT <select-list>
                    FROM <ident>
                    {<join-clause>}
                    [WHERE <condition>]

<select-list>   ::= '*'
                  | <column-ref> {',' <column-ref>}

<column-ref>    ::= <ident> ['.' <ident>]

<join-clause>   ::= JOIN <ident> ON <column-ref> '=' <column-ref>

<condition>     ::= <column-ref> <comparison-op> <literal>

<comparison-op> ::= '=' | '!=' | '<' | '>' | '<=' | '>='

<literal>       ::= <number> | <string>
```

Notes:
- `JOIN` is inner join only; ON accepts one equality between two columns.
- `WHERE` is a single predicate — no `AND` / `OR` yet.
- `WHERE`'s right-hand side must be a literal; column-vs-column is rejected.

## CREATE TABLE

```
<create-table-stmt>
                ::= CREATE TABLE <ident> '(' <column-def> {',' <column-def>} ')'

<column-def>    ::= <ident> <type-name> [NOT NULL]

<type-name>     ::= <ident>
```

Notes:
- The column list must contain at least one column.
- `<type-name>` is any identifier; the analyzer is responsible for
  mapping the surface text (e.g. `INT`, `BIGINT`, `BOOL`, `TEXT`) to a
  concrete `Type`. The parser preserves the original casing.
- Columns are nullable by default. `NOT NULL` is the only column
  constraint accepted.

## INSERT

```
<insert-stmt>   ::= INSERT INTO <ident>
                    [<insert-column-list>]
                    VALUES <values-row> {',' <values-row>}

<insert-column-list>
                ::= '(' <ident> {',' <ident>} ')'

<values-row>    ::= '(' <insert-literal> {',' <insert-literal>} ')'

<insert-literal>
                ::= <number> | <string> | NULL
```

Notes:
- The optional column list scopes the values to specific columns; when
  omitted, the analyzer treats the row values as schema-column order.
- At least one row is required after `VALUES`.
- `NULL` is a literal in this position only (the analyzer rejects it
  against a non-nullable column). Numeric and string literals carry the
  same flags as in `WHERE`.

## Lexical rules

- **Identifiers**: letter or `_`, then letters, digits, or `_`.
- **Numbers**: one or more decimal digits. No sign, no decimal point.
- **Strings**: single-quoted; no escape sequences (a `'` ends the string).
- **Keywords** (case-insensitive, reserved): `SELECT`, `FROM`, `WHERE`,
  `JOIN`, `ON`, `CREATE`, `TABLE`, `INSERT`, `INTO`, `VALUES`, `NOT`,
  `NULL`.
- **Punctuation**: `,` `*` `.` `(` `)`.
- **Operators**: `=` `!=` `<` `>` `<=` `>=`.

## Not yet supported

- DML: `UPDATE`, `DELETE`.
- DDL beyond `CREATE TABLE`: `DROP`, `ALTER`, indexes, constraints other
  than `NOT NULL`.
- `ORDER BY`, `LIMIT`, `GROUP BY`, aggregates.
- Expressions in the SELECT list (only column references are accepted).
- Compound `WHERE` predicates (`AND` / `OR`), parenthesised conditions,
  `IN`, `LIKE`, `BETWEEN`, `IS NULL`.
- Table aliases (so a table cannot appear twice in one query).
- `INSERT ... SELECT`.
- `TRUE` / `FALSE` literals (use `0` / `1` against `Bool` columns).
- Numeric types beyond integer: floats, decimals, dates.
- Comments (`--`, `/* */`).
