#include "parser.h"

#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

// Pretty-print a parsed SelectQuery to stdout in a stable, debuggable format.
static void printQuery(const SelectQuery& q) {
    std::cout << "  columns: ";
    if (q.select_all) {
        std::cout << "*";
    } else {
        for (size_t i = 0; i < q.columns.size(); ++i) {
            if (i) std::cout << ", ";
            std::cout << q.columns[i];
        }
    }
    std::cout << "\n  table:   " << q.table << "\n";

    for (const auto& j : q.joins) {
        std::cout << "  join:    " << j.table
                  << " ON " << j.left << " = " << j.right << "\n";
    }

    if (q.where) {
        const auto& w = *q.where;
        std::cout << "  where:   " << w.column << " " << opToString(w.op) << " ";
        if (w.value_is_string) std::cout << "'" << w.value << "'";
        else                   std::cout << w.value;
        std::cout << "\n";
    } else {
        std::cout << "  where:   (none)\n";
    }
}

// Driver: parse a handful of example queries and print the resulting AST.
// The last query is intentionally malformed to exercise the error path.
int main() {
    const std::vector<std::string> queries = {
        "SELECT id, name FROM users WHERE age > 18",
        "SELECT * FROM products",
        "SELECT name FROM users,posts WHERE name = 'alice'",
        "select email, id from accounts where status != 'banned'",
        "SELECT u.id, u.name, p.title FROM users JOIN posts ON u.id = p.user_id",
        "SELECT * FROM a JOIN b ON a.x = b.x JOIN c ON b.y = c.y WHERE c.z > 0",
        "SELECT FROM users",
    };

    for (const auto& sql : queries) {
        std::cout << "SQL: " << sql << "\n";
        try {
            Parser p(sql);
            SelectQuery q = p.parse();
            printQuery(q);
        } catch (const std::exception& e) {
            // Lex or parse error — keep going so remaining examples still run.
            std::cout << "  error:   " << e.what() << "\n";
        }
        std::cout << "\n";
    }

    return 0;
}
