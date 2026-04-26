#pragma once

#include <optional>
#include <string>
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

// Parses a single SELECT statement from a SQL string.
// Construction tokenizes; parse() walks the tokens to produce a SelectQuery.
// Throws std::runtime_error on any lex or parse error.
class Parser {
public:
    explicit Parser(std::string sql);
    SelectQuery parse();

private:
    // Token kinds the lexer emits. Keywords (Select/From/Where) are
    // separated from generic Identifier so the parser can match on kind alone.
    enum class Tok {
        Select, From, Where, Join, On,
        Identifier, Number, String,
        Comma, Star, Dot, Op,
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

    // Grammar productions; each consumes tokens and writes into `q`.
    void parseColumns(SelectQuery& q);
    void parseJoins(SelectQuery& q);
    void parseWhere(SelectQuery& q);

    // Reads an identifier, optionally followed by `.identifier`, and returns
    // the joined surface form (e.g. "id" or "users.id"). Used everywhere a
    // column may appear so qualified names work uniformly.
    std::string parseColumnRef();
};
