#include "tbcad/textcmd/textcmd.h"

namespace tbcad::textcmd {

std::string markupToEditable(std::string_view m) {
    std::string out;
    for (std::size_t i = 0; i < m.size(); ++i) {
        if (m[i] == '\\' && i + 1 < m.size()) {
            if (m[i + 1] == 'P')
                out += '\n';
            else {
                out += m[i];
                out += m[i + 1];
            }
            ++i;
        } else {
            out += m[i];
        }
    }
    return out;
}

std::string editableToMarkup(std::string_view e) {
    std::string out;
    for (std::size_t i = 0; i < e.size(); ++i) {
        char c = e[i];
        if (c == '\r') {
            if (i + 1 < e.size() && e[i + 1] == '\n')
                ++i;
            out += "\\P";
        } else if (c == '\n') {
            out += "\\P";
        } else if (c == '\\' && i + 1 < e.size()) {
            out += c;
            out += e[++i];
        } else {
            out += c;
        }
    }
    return out;
}

}  // namespace tbcad::textcmd
