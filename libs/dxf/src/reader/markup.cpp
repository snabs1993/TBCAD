// MTEXT markup conversion (DXF -> TBCAD markup, see docs/dxf-import.md).
//
// Kept:   line breaks (\P becomes a newline), \\ \{ \}, bold/italic taken from \f font codes
//         as {\b1;..} / {\i1;..}, \H<height>; (scaled to mm, "2x" relative form unchanged) and
//         \C<aci>; (mapped to the TBCAD palette).
// Turned into plain text: \S stacks ("1/2"), \~ (space), %% codes.
// Dropped (and counted): every other formatting code.
#include <cctype>
#include <cstdio>
#include <cstdlib>

#include "importer.h"
#include "tbcad/dxf/encoding.h"

namespace tbcad::dxf::detail {

namespace {

struct BraceGroup {
    std::size_t outPos;
    bool hadCode;
};

std::string formatNumber(double v) {
    char buf[40];
    std::snprintf(buf, sizeof buf, "%.6g", v);
    return buf;
}

// The settings of a \p code with their lengths multiplied by k (the alignment letter, and the
// "sb"/"sa"/"i"/"l"/"r"/"t" prefixes stay).
std::string scaleParagraph(std::string_view arg, double k) {
    std::string out;
    std::size_t at = 0;
    bool first = true;
    while (at <= arg.size()) {
        std::size_t comma = arg.find(',', at);
        std::string_view tok =
            arg.substr(at, comma == std::string_view::npos ? std::string_view::npos : comma - at);
        at = comma == std::string_view::npos ? arg.size() + 1 : comma + 1;
        if (tok.empty())
            continue;
        std::size_t pre = 0;
        if (first && tok[0] == 'x')
            ++pre;
        first = false;
        if (tok.compare(pre, 1, "q") != 0) {
            if (tok.compare(pre, 2, "sb") == 0 || tok.compare(pre, 2, "sa") == 0)
                pre += 2;
            else if (pre < tok.size() && std::isalpha(static_cast<unsigned char>(tok[pre])))
                ++pre;
            const std::string num(tok.substr(pre));
            char* end = nullptr;
            const double v = std::strtod(num.c_str(), &end);
            if (end != num.c_str() && *end == 0) {
                if (!out.empty())
                    out.push_back(',');
                out += std::string(tok.substr(0, pre)) + formatNumber(v * k);
                continue;
            }
        }
        if (!out.empty())
            out.push_back(',');
        out += tok;
    }
    return out;
}

}  // namespace

std::string convertMText(std::string_view s, double unitScale, std::map<char, int>& dropped) {
    std::string out;
    out.reserve(s.size());
    std::vector<BraceGroup> stack;

    auto markCode = [&] {
        if (!stack.empty())
            stack.back().hadCode = true;
    };
    // Reads up to and including the next ';' (or the end of input).
    auto param = [&](std::size_t& i) {
        std::size_t start = i;
        while (i < s.size() && s[i] != ';')
            ++i;
        std::string_view p = s.substr(start, i - start);
        if (i < s.size())
            ++i;  // ';'
        return p;
    };

    std::size_t i = 0;
    while (i < s.size()) {
        char c = s[i];
        if (c == '{') {
            stack.push_back({out.size(), false});
            out.push_back('{');
            ++i;
        } else if (c == '}') {
            if (!stack.empty()) {
                BraceGroup g = stack.back();
                stack.pop_back();
                if (g.hadCode) {
                    out.push_back('}');
                } else {
                    out.erase(g.outPos, 1);
                    // Positions of enclosing groups are before g.outPos, so they stay valid.
                }
            }
            ++i;
        } else if (c == '\\' && i + 1 < s.size()) {
            char k = s[i + 1];
            i += 2;
            switch (k) {
                case 'P':
                case 'p':
                    if (k == 'P') {
                        out.push_back('\n');
                    } else {
                        // \pxi-3,l3,qj,t4;: paragraph settings. Lengths are in drawing units.
                        out += "\\p";
                        out += scaleParagraph(param(i), unitScale);
                        out.push_back(';');
                        markCode();
                    }
                    break;
                case '\\':
                    out += "\\\\";
                    break;
                case '{':
                    out += "\\{";
                    break;
                case '}':
                    out += "\\}";
                    break;
                case '~':
                    out += "\xC2\xA0";  // no-break space
                    break;
                case 'N':
                    out += "\\N";
                    markCode();
                    break;
                case 'f':
                case 'F': {
                    std::string_view p = param(i);
                    bool bold = p.find("|b1") != std::string_view::npos;
                    bool italic = p.find("|i1") != std::string_view::npos;
                    if (bold) {
                        out += "\\b1;";
                        markCode();
                    }
                    if (italic) {
                        out += "\\i1;";
                        markCode();
                    }
                    break;
                }
                case 'H': {
                    std::string_view p = param(i);
                    std::string num(p);
                    char* end = nullptr;
                    double v = std::strtod(num.c_str(), &end);
                    if (end != num.c_str() && v > 0) {
                        out += "\\H";
                        if (end && *end == 'x')
                            out += formatNumber(v) + "x;";
                        else
                            out += formatNumber(v * unitScale) + ";";
                        markCode();
                    } else {
                        ++dropped['H'];
                    }
                    break;
                }
                case 'C': {
                    std::string_view p = param(i);
                    int aci = std::atoi(std::string(p).c_str());
                    if (aci >= 1 && aci <= 255) {
                        out += "\\C" + std::to_string(mapAci(aci).indexValue()) + ";";
                        markCode();
                    } else {
                        ++dropped['C'];
                    }
                    break;
                }
                case 'S': {
                    // Stacked fraction: kept (TBCAD markup has the same \S<num>/<den>; code).
                    std::string_view p = param(i);
                    out += "\\S";
                    out += p;
                    out += ';';
                    markCode();
                    break;
                }
                case 'A':
                case 'Q':
                case 'T':
                case 'W':
                case 'X':
                    param(i);
                    ++dropped[k];
                    break;
                case 'L':
                case 'l':
                case 'O':
                case 'o':
                    out.push_back('\\');
                    out.push_back(k);
                    markCode();
                    break;
                case 'K':
                case 'k':
                    ++dropped[k];
                    break;
                default:
                    // Unknown escape: keep the character itself.
                    out.push_back(k);
                    break;
            }
        } else {
            out.push_back(c);
            ++i;
        }
    }
    // Unclosed groups: close those that carry a code, remove the brace of the others. Work from
    // the innermost so earlier positions stay valid.
    while (!stack.empty()) {
        BraceGroup g = stack.back();
        stack.pop_back();
        if (g.hadCode) {
            out.push_back('}');
        } else {
            out.erase(g.outPos, 1);
        }
    }
    return replacePercentCodes(out);
}

}  // namespace tbcad::dxf::detail
