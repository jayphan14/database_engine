#include "parser.h"

#include "src/util/string.h"

#include <cctype>
#include <stdexcept>
#include <utility>

const char* opToString(Op op) {
    switch (op) {
        case Op::Eq:  return "=";
        case Op::Neq: return "!=";
        case Op::Lt:  return "<";
        case Op::Gt:  return ">";
        case Op::Leq: return "<=";
        case Op::Geq: return ">=";
    }
    return "?";
}

// Tokenize as soon as constructed
// so parse() can just read the tokens
Parser::Parser(std::string sql) : src_(std::move(sql)) {
    tokenize();
}

// Single-pass lexer. Walks `src_` and appends to `tokens_`, finishing with Tok::End.
void Parser::tokenize() {
    size_t i = 0;
    const size_t n = src_.size();

    auto isIdentStart = [](char c) { return std::isalpha(static_cast<unsigned char>(c)) || c == '_'; };
    auto isIdentCont  = [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; };

    while (i < n) {
        char c = src_[i];

        // Whitespace is not significant — skip it.
        if (std::isspace(static_cast<unsigned char>(c))) { ++i; continue; }

        // Identifier or keyword: read the full word, then check if it's a reserved keyword.
        if (isIdentStart(c)) {
            size_t start = i;
            while (i < n && isIdentCont(src_[i])) ++i;
            std::string word = src_.substr(start, i - start);
            std::string upper = util::toUpper(word);
            if      (upper == "SELECT") tokens_.push_back({Tok::Select, word, Op::Eq});
            else if (upper == "FROM")   tokens_.push_back({Tok::From,   word, Op::Eq});
            else if (upper == "WHERE")  tokens_.push_back({Tok::Where,  word, Op::Eq});
            else if (upper == "JOIN")   tokens_.push_back({Tok::Join,   word, Op::Eq});
            else if (upper == "ON")     tokens_.push_back({Tok::On,     word, Op::Eq});
            else if (upper == "CREATE") tokens_.push_back({Tok::Create, word, Op::Eq});
            else if (upper == "TABLE")  tokens_.push_back({Tok::Table,  word, Op::Eq});
            else if (upper == "INSERT") tokens_.push_back({Tok::Insert, word, Op::Eq});
            else if (upper == "INTO")   tokens_.push_back({Tok::Into,   word, Op::Eq});
            else if (upper == "VALUES") tokens_.push_back({Tok::Values, word, Op::Eq});
            else if (upper == "NOT")    tokens_.push_back({Tok::Not,    word, Op::Eq});
            else if (upper == "NULL")   tokens_.push_back({Tok::Null,   word, Op::Eq});
            else                        tokens_.push_back({Tok::Identifier, word, Op::Eq});
            continue;
        }

        // Integer literal — consecutive digits.
        if (std::isdigit(static_cast<unsigned char>(c))) {
            size_t start = i;
            while (i < n && std::isdigit(static_cast<unsigned char>(src_[i]))) ++i;
            tokens_.push_back({Tok::Number, src_.substr(start, i - start), Op::Eq});
            continue;
        }

        // Single-quoted string literal. No escape handling — a quote ends it.
        if (c == '\'') {
            ++i;
            size_t start = i;
            while (i < n && src_[i] != '\'') ++i;
            if (i >= n) throw std::runtime_error("unterminated string literal");
            std::string val = src_.substr(start, i - start);
            ++i;  // consume the closing quote
            tokens_.push_back({Tok::String, val, Op::Eq});
            continue;
        }

        // Single-char punctuation.
        if (c == ',') { tokens_.push_back({Tok::Comma,  ",", Op::Eq}); ++i; continue; }
        if (c == '*') { tokens_.push_back({Tok::Star,   "*", Op::Eq}); ++i; continue; }
        if (c == '.') { tokens_.push_back({Tok::Dot,    ".", Op::Eq}); ++i; continue; }
        if (c == '(') { tokens_.push_back({Tok::Lparen, "(", Op::Eq}); ++i; continue; }
        if (c == ')') { tokens_.push_back({Tok::Rparen, ")", Op::Eq}); ++i; continue; }

        // Comparison operators. `<` and `>` may be 1 or 2 chars (`<` vs `<=`),
        // so peek ahead before committing to a length.
        if (c == '=') { tokens_.push_back({Tok::Op, "=", Op::Eq}); ++i; continue; }
        if (c == '!') {
            if (i + 1 < n && src_[i+1] == '=') { tokens_.push_back({Tok::Op, "!=", Op::Neq}); i += 2; continue; }
            throw std::runtime_error("expected '=' after '!'");
        }
        if (c == '<') {
            if (i + 1 < n && src_[i+1] == '=') { tokens_.push_back({Tok::Op, "<=", Op::Leq}); i += 2; continue; }
            tokens_.push_back({Tok::Op, "<", Op::Lt}); ++i; continue;
        }
        if (c == '>') {
            if (i + 1 < n && src_[i+1] == '=') { tokens_.push_back({Tok::Op, ">=", Op::Geq}); i += 2; continue; }
            tokens_.push_back({Tok::Op, ">", Op::Gt}); ++i; continue;
        }

        throw std::runtime_error(std::string("unexpected character: ") + c);
    }

    // Sentinel so the parser can match `Tok::End` instead of bounds-checking.
    tokens_.push_back({Tok::End, "", Op::Eq});
}

// Look at the next token without consuming it.
const Parser::Token& Parser::peek() const { return tokens_[pos_]; }

// Consume and return the next token.
const Parser::Token& Parser::consume() { return tokens_[pos_++]; }

// Consume the next token if it matches `kind`, else throw with a helpful message.
// `what` is the human-readable thing we expected, used in the error text.
const Parser::Token& Parser::expect(Tok kind, const char* what) {
    if (peek().kind != kind) {
        std::string got = peek().text.empty() ? "<end>" : peek().text;
        throw std::runtime_error(std::string("expected ") + what + ", got '" + got + "'");
    }
    return consume();
}

// Top-level dispatch: SELECT / CREATE TABLE / INSERT INTO. The first
// keyword picks the production; each sub-parser leaves the tokens
// positioned just past its statement, and parse() rejects trailing input.
Statement Parser::parse() {
    Statement out;
    switch (peek().kind) {
        case Tok::Select: out = parseSelect();      break;
        case Tok::Create: out = parseCreateTable(); break;
        case Tok::Insert: out = parseInsert();      break;
        default:
            throw std::runtime_error(
                "expected SELECT, CREATE, or INSERT at start of statement");
    }
    expect(Tok::End, "end of input");
    return out;
}

// SELECT <columns> FROM <table> {JOIN <table> ON <col> = <col>} [WHERE <condition>]
SelectQuery Parser::parseSelect() {
    SelectQuery q;

    expect(Tok::Select, "SELECT");
    parseColumns(q);
    expect(Tok::From, "FROM");
    q.table = expect(Tok::Identifier, "table name").text;

    // Zero or more JOINs may follow the FROM table, before WHERE.
    parseJoins(q);

    // WHERE clause is optional.
    if (peek().kind == Tok::Where) {
        consume();
        parseWhere(q);
    }
    return q;
}

// A column reference: <ident> or <ident>.<ident>. Returned as a single
// string with the dot preserved so downstream code can split if it cares.
std::string Parser::parseColumnRef() {
    std::string name = expect(Tok::Identifier, "column name").text;
    if (peek().kind == Tok::Dot) {
        consume();
        name += ".";
        name += expect(Tok::Identifier, "column name after '.'").text;
    }
    return name;
}

// Column list: either `*` or a comma-separated list of identifiers.
void Parser::parseColumns(SelectQuery& q) {
    if (peek().kind == Tok::Star) {
        consume();
        q.select_all = true;
        return;
    }

    // At least one column is required.
    q.columns.push_back(parseColumnRef());
    while (peek().kind == Tok::Comma) {
        consume();
        q.columns.push_back(parseColumnRef());
    }
}

// Zero or more `JOIN <table> ON <col> = <col>` clauses.
// Only `=` is accepted in ON — anything else would silently change semantics
// (e.g. inequality joins), so reject loudly until we explicitly support it.
void Parser::parseJoins(SelectQuery& q) {
    while (peek().kind == Tok::Join) {
        consume();
        Join j;
        j.table = expect(Tok::Identifier, "table name after JOIN").text;
        expect(Tok::On, "ON");
        j.left = parseColumnRef();
        const Token& opTok = expect(Tok::Op, "'=' in ON clause");
        if (opTok.op != Op::Eq) {
            throw std::runtime_error("only '=' is supported in ON clause");
        }
        j.right = parseColumnRef();
        q.joins.push_back(std::move(j));
    }
}

// WHERE body: <column> <op> <literal>. Single predicate only — no AND/OR.
void Parser::parseWhere(SelectQuery& q) {
    Condition c;
    c.column = parseColumnRef();

    // The operator token carries its semantic Op in `op` — copy it across.
    const Token& opTok = expect(Tok::Op, "comparison operator");
    c.op = opTok.op;

    // Literal must be a number or a string. Tag which one for the AST consumer.
    const Token& val = peek();
    if (val.kind == Tok::Number) {
        c.value = val.text;
        c.value_is_string = false;
        consume();
    } else if (val.kind == Tok::String) {
        c.value = val.text;
        c.value_is_string = true;
        consume();
    } else {
        throw std::runtime_error("expected number or string literal in WHERE");
    }

    q.where = c;
}

// CREATE TABLE <name> (<col-def>{, <col-def>})
CreateTableStmt Parser::parseCreateTable() {
    CreateTableStmt s;
    expect(Tok::Create, "CREATE");
    expect(Tok::Table,  "TABLE");
    s.table = expect(Tok::Identifier, "table name").text;

    expect(Tok::Lparen, "'(' before column list");
    // At least one column is required.
    s.columns.push_back(parseColumnDef());
    while (peek().kind == Tok::Comma) {
        consume();
        s.columns.push_back(parseColumnDef());
    }
    expect(Tok::Rparen, "')' after column list");
    return s;
}

// One CREATE TABLE column: <name> <type-keyword> [NOT NULL]
// The type keyword tokenizes as Identifier; the analyzer maps the surface
// text (e.g. "INT", "BIGINT") to a Type, mirroring how WHERE literals
// stay as raw text until the analyzer types them.
ColumnDef Parser::parseColumnDef() {
    ColumnDef c;
    c.name      = expect(Tok::Identifier, "column name").text;
    c.type_name = expect(Tok::Identifier, "column type").text;

    // Optional `NOT NULL`. NULL alone is not accepted as a constraint —
    // columns are nullable by default, so writing it would only be noise.
    if (peek().kind == Tok::Not) {
        consume();
        expect(Tok::Null, "NULL after NOT");
        c.nullable = false;
    }
    return c;
}

// INSERT INTO <table> [(<col>{,<col>})] VALUES (<lit>{,<lit>}){, (...)}
InsertStmt Parser::parseInsert() {
    InsertStmt s;
    expect(Tok::Insert, "INSERT");
    expect(Tok::Into,   "INTO");
    s.table = expect(Tok::Identifier, "table name").text;

    // Optional column list. A bare `(` here means the user is naming target
    // columns; otherwise we go straight to VALUES.
    if (peek().kind == Tok::Lparen) {
        s.columns = parseInsertColumnList();
    }

    expect(Tok::Values, "VALUES");
    // At least one row is required.
    s.rows.push_back(parseInsertRow());
    while (peek().kind == Tok::Comma) {
        consume();
        s.rows.push_back(parseInsertRow());
    }
    return s;
}

// `(col{, col})` — used only for INSERT's optional target column list.
// Only bare identifiers; qualified `t.c` form makes no sense for a target.
std::vector<std::string> Parser::parseInsertColumnList() {
    std::vector<std::string> out;
    expect(Tok::Lparen, "'(' before INSERT column list");
    out.push_back(expect(Tok::Identifier, "column name").text);
    while (peek().kind == Tok::Comma) {
        consume();
        out.push_back(expect(Tok::Identifier, "column name").text);
    }
    expect(Tok::Rparen, "')' after INSERT column list");
    return out;
}

// `(<lit>{, <lit>})` — one row of values for INSERT.
std::vector<InsertLiteral> Parser::parseInsertRow() {
    std::vector<InsertLiteral> out;
    expect(Tok::Lparen, "'(' before VALUES row");
    out.push_back(parseInsertLiteral());
    while (peek().kind == Tok::Comma) {
        consume();
        out.push_back(parseInsertLiteral());
    }
    expect(Tok::Rparen, "')' after VALUES row");
    return out;
}

// One literal cell: number, single-quoted string, or NULL keyword.
InsertLiteral Parser::parseInsertLiteral() {
    const Token& t = peek();
    InsertLiteral lit;
    if (t.kind == Tok::Number) {
        lit.text = t.text;
        consume();
    } else if (t.kind == Tok::String) {
        lit.text = t.text;
        lit.is_string = true;
        consume();
    } else if (t.kind == Tok::Null) {
        lit.is_null = true;
        consume();
    } else {
        throw std::runtime_error(
            "expected number, string, or NULL in VALUES");
    }
    return lit;
}
