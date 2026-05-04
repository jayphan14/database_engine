#include "parser.h"

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

// Used to fold keywords case-insensitively (so `select` and `SELECT` match).
static std::string toUpper(const std::string& s) {
    std::string out = s;
    for (char& c : out) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return out;
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
            std::string upper = toUpper(word);
            if (upper == "SELECT")      tokens_.push_back({Tok::Select, word, Op::Eq});
            else if (upper == "FROM")   tokens_.push_back({Tok::From,   word, Op::Eq});
            else if (upper == "WHERE")  tokens_.push_back({Tok::Where,  word, Op::Eq});
            else if (upper == "JOIN")   tokens_.push_back({Tok::Join,   word, Op::Eq});
            else if (upper == "ON")     tokens_.push_back({Tok::On,     word, Op::Eq});
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
        if (c == ',') { tokens_.push_back({Tok::Comma, ",", Op::Eq}); ++i; continue; }
        if (c == '*') { tokens_.push_back({Tok::Star,  "*", Op::Eq}); ++i; continue; }
        if (c == '.') { tokens_.push_back({Tok::Dot,   ".", Op::Eq}); ++i; continue; }

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

// Top-level production:
//   SELECT <columns> FROM <table> {JOIN <table> ON <col> = <col>} [WHERE <condition>]
SelectQuery Parser::parse() {
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

    // Reject trailing tokens — the whole input must be a single statement.
    expect(Tok::End, "end of input");
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
