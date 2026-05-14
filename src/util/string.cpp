#include "src/util/string.h"

#include <cctype>

namespace util {

std::string toUpper(const std::string& s) {
    std::string out = s;
    for (char& c : out) {
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    return out;
}

}  // namespace util
