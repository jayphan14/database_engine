#pragma once

#include <optional>
#include <string>
#include <variant>
#include <vector>

// Comparison operators allowed in a WHERE clause.
enum class Op { Eq, Neq, Lt, Gt, Leq, Geq };

// Render an Op back to its SQL surface form (e.g. Op::Neq -> "!=").
const char* opToString(Op op);

// One predicate of the form: <column> <op> <literal>.
// `value_is_string` distinguishes a quoted literal ('alice') from a numeric one (18),
// since the raw text alone can't tell them apart later.
struct Condition {
    std::string column;
    Op op;
    std::string value;
    bool value_is_string;
};

// One JOIN clause: `JOIN <table> ON <left> = <right>`.
// Only equality on a pair of columns is supported; both sides may be
// qualified (e.g. `users.id`) and are stored verbatim including the dot.
struct Join {
    std::string table;
    std::string left;
    std::string right;
};

// AST root for a parsed SELECT statement.
// If `select_all` is true, `columns` is empty and the query is `SELECT *`.
// `where` is unset when the query has no WHERE clause.
struct SelectQuery {
    std::vector<std::string> columns;
    bool select_all = false;
    std::string table;
    std::vector<Join> joins;
    std::optional<Condition> where;
};

// One column declaration inside a CREATE TABLE column list.
// `type_name` is the surface keyword as written (e.g. "INT", "BIGINT",
// "TEXT") — the analyzer maps it to a Type. `nullable` defaults to true;
// `NOT NULL` flips it to false.
struct ColumnDef {
    std::string name;
    std::string type_name;
    bool nullable = true;
};

// AST for `CREATE TABLE <name> (<col-def>{, <col-def>})`.
struct CreateTableStmt {
    std::string table;
    std::vector<ColumnDef> columns;
};

// One literal cell in an INSERT VALUES list. Mirrors the WHERE pattern:
// the parser stores the raw text plus enough flags for the analyzer to
// pick the right typed Value. When `is_null` is true the other fields
// are unused.
struct InsertLiteral {
    std::string text;
    bool is_string = false;
    bool is_null = false;
};

// AST for `INSERT INTO <name> [(<cols>)] VALUES (<lit>{,<lit>}){, (...)}`.
// `columns` is empty when the user omitted the column list, meaning
// "values are in schema column order"; otherwise it lists the explicit
// target columns in source order. `rows` is non-empty (the parser
// rejects a trailing VALUES with no row).
struct InsertStmt {
    std::string table;
    std::vector<std::string> columns;
    std::vector<std::vector<InsertLiteral>> rows;
};

// A parsed top-level SQL statement. The parser dispatches on the first
// keyword and produces exactly one of these alternatives.
using Statement = std::variant<SelectQuery, CreateTableStmt, InsertStmt>;

// Parses a single SQL statement from a SQL string.
// Construction tokenizes; parse() walks the tokens to produce a Statement.
// Throws std::runtime_error on any lex or parse error.
class Parser {
public:
    explicit Parser(std::string sql);
    Statement parse();

private:
    // Token kinds the lexer emits. Keywords are separated from generic
    // Identifier so the parser can match on kind alone.
    enum class Tok {
        Select, From, Where, Join, On,
        Create, Table, Insert, Into, Values,
        Not, Null,
        Identifier, Number, String,
        Comma, Star, Dot, Op,
        Lparen, Rparen,
        End
    };

    // A lexed token. `op` is only meaningful when `kind == Tok::Op`.
    struct Token {
        Tok kind;
        std::string text;
        ::Op op;
    };

    std::string src_;
    std::vector<Token> tokens_;
    size_t pos_ = 0;

    // Lex `src_` into `tokens_`. Called once from the constructor.
    void tokenize();

    // Token-stream helpers used by the recursive-descent parser.
    const Token& peek() const;
    const Token& consume();
    const Token& expect(Tok kind, const char* what);

    // Top-level dispatch: SELECT / CREATE TABLE / INSERT.
    SelectQuery     parseSelect();
    CreateTableStmt parseCreateTable();
    InsertStmt      parseInsert();

    // SELECT sub-productions.
    void parseColumns(SelectQuery& q);
    void parseJoins(SelectQuery& q);
    void parseWhere(SelectQuery& q);

    // Reads an identifier, optionally followed by `.identifier`, and returns
    // the joined surface form (e.g. "id" or "users.id"). Used everywhere a
    // column may appear so qualified names work uniformly.
    std::string parseColumnRef();

    // CREATE TABLE sub-productions.
    ColumnDef parseColumnDef();

    // INSERT sub-productions.
    std::vector<std::string>   parseInsertColumnList();
    std::vector<InsertLiteral> parseInsertRow();
    InsertLiteral              parseInsertLiteral();
};
