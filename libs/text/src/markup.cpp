#include <algorithm>
#include <charconv>
#include <cmath>

#include "text_internal.h"

namespace tbcad::text {

namespace {

struct Frame {
    Attr attr;
};

// Reads "...;" starting at i (after the backslash and the code letter). Returns the argument
// and moves i past the ';'. Empty optional when there is no ';' before the end.
std::optional<std::string_view> readArg(std::string_view s, std::size_t& i) {
    std::size_t semi = s.find(';', i);
    if (semi == std::string_view::npos)
        return std::nullopt;
    std::string_view arg = s.substr(i, semi - i);
    i = semi + 1;
    return arg;
}

// "xi-3,l3,r2,qj,t4,8,sb1,sa1": the settings of a \p code. Unknown items are skipped.
ParaProps parseParaProps(std::string_view arg) {
    ParaProps pp;
    if (!arg.empty() && arg[0] == 'x')
        arg.remove_prefix(1);
    auto number = [](std::string_view t, double& v) {
        if (t.empty())
            return false;
        auto [p, ec] = std::from_chars(t.data(), t.data() + t.size(), v);
        return ec == std::errc{} && p == t.data() + t.size() && std::isfinite(v);
    };
    bool inTabs = false;
    std::size_t at = 0;
    while (at <= arg.size()) {
        std::size_t comma = arg.find(',', at);
        std::string_view tok =
            arg.substr(at, comma == std::string_view::npos ? std::string_view::npos : comma - at);
        at = comma == std::string_view::npos ? arg.size() + 1 : comma + 1;
        if (tok.empty())
            continue;
        double v = 0;
        if (number(tok, v)) {  // a bare number continues the tab list
            if (inTabs)
                pp.tabs.push_back(v);
            continue;
        }
        inTabs = false;
        if (tok.size() >= 2 && tok[0] == 's' && (tok[1] == 'b' || tok[1] == 'a')) {
            if (number(tok.substr(2), v))
                (tok[1] == 'b' ? pp.spaceBefore : pp.spaceAfter) = std::max(0.0, v);
        } else if (tok[0] == 'q' && tok.size() >= 2) {
            switch (tok[1]) {
                case 'l':
                    pp.align = ParaProps::Align::Left;
                    break;
                case 'c':
                    pp.align = ParaProps::Align::Center;
                    break;
                case 'r':
                    pp.align = ParaProps::Align::Right;
                    break;
                case 'j':
                    pp.align = ParaProps::Align::Justify;
                    break;
                case 'd':
                    pp.align = ParaProps::Align::Distribute;
                    break;
                default:
                    break;
            }
        } else if (tok[0] == 'i' && number(tok.substr(1), v)) {
            pp.firstIndent = v;
        } else if (tok[0] == 'l' && number(tok.substr(1), v)) {
            pp.leftIndent = v;
        } else if (tok[0] == 'r' && number(tok.substr(1), v)) {
            pp.rightIndent = std::max(0.0, v);
        } else if (tok[0] == 't') {
            inTabs = true;
            if (number(tok.substr(1), v))
                pp.tabs.push_back(v);
        }
    }
    std::sort(pp.tabs.begin(), pp.tabs.end());
    return pp;
}

}  // namespace

std::vector<Piece> parseMarkup(std::string_view s, double baseHeight) {
    std::vector<Piece> out;
    std::vector<Frame> stack;
    Attr cur;
    cur.height = baseHeight;
    std::string text;

    auto flush = [&]() {
        if (!text.empty()) {
            Piece p;
            p.text = std::move(text);
            p.attr = cur;
            out.push_back(std::move(p));
            text.clear();
        }
    };

    for (std::size_t i = 0; i < s.size();) {
        char c = s[i];
        if (c == '{') {
            flush();
            stack.push_back({cur});
            ++i;
        } else if (c == '}') {
            if (!stack.empty()) {
                flush();
                cur = stack.back().attr;
                stack.pop_back();
            }
            ++i;
        } else if (c == '\n' || c == '\r') {
            // A raw line break is a paragraph mark too (the DXF importer produces them).
            flush();
            Piece p;
            p.newline = true;
            p.attr = cur;
            out.push_back(std::move(p));
            i += (c == '\r' && i + 1 < s.size() && s[i + 1] == '\n') ? 2 : 1;
        } else if (c == '\\' && i + 1 < s.size()) {
            char k = s[i + 1];
            if (k == '\\' || k == '{' || k == '}') {
                text += k;
                i += 2;
            } else if (k == 'L' || k == 'l' || k == 'O' || k == 'o') {
                flush();
                if (k == 'L' || k == 'l')
                    cur.underline = k == 'L';
                else
                    cur.overline = k == 'O';
                i += 2;
            } else if (k == 'p') {
                // Paragraph settings: \pxi-3,l3,r2,qj,t4,8,sb1,sa1;
                std::size_t j = i + 2;
                auto arg = readArg(s, j);
                if (!arg) {
                    text += c;
                    ++i;
                    continue;
                }
                flush();
                Piece p;
                p.attr = cur;
                p.para = parseParaProps(*arg);
                out.push_back(std::move(p));
                i = j;
            } else if (k == 'N') {
                flush();
                Piece p;
                p.attr = cur;
                p.columnBreak = true;
                out.push_back(std::move(p));
                i += 2;
            } else if (k == '~') {
                text += "\xC2\xA0";  // no-break space
                i += 2;
            } else if (k == 'P') {
                flush();
                Piece p;
                p.newline = true;
                p.attr = cur;
                out.push_back(std::move(p));
                i += 2;
            } else if (k == 'S') {
                // \S<num>/<den>;  also # (diagonal) and ^ (no bar). An escaped separator
                // (\/ \# \^) is text.
                std::size_t j = i + 2;
                std::string num, den;
                char bar = 0;
                bool closed = false;
                for (; j < s.size(); ++j) {
                    const char ch = s[j];
                    if (ch == '\\' && j + 1 < s.size()) {
                        (bar ? den : num) += s[++j];
                    } else if (ch == ';') {
                        closed = true;
                        break;
                    } else if (!bar && (ch == '/' || ch == '#' || ch == '^')) {
                        bar = ch;
                    } else {
                        (bar ? den : num) += ch;
                    }
                }
                if (!closed || !bar) {
                    text += c;  // not a fraction: keep the backslash literally
                    ++i;
                    continue;
                }
                flush();
                Piece p;
                p.text = num + "/" + den;
                p.attr = cur;
                p.stacked = true;
                p.num = std::move(num);
                p.den = std::move(den);
                p.bar = bar;
                out.push_back(std::move(p));
                i = j + 1;
            } else if (k == 'b' || k == 'i' || k == 'H' || k == 'C') {
                std::size_t j = i + 2;
                auto arg = readArg(s, j);
                if (!arg) {
                    text += c;  // no ';': keep the backslash literally
                    ++i;
                    continue;
                }
                flush();
                if (k == 'b') {
                    cur.bold = !arg->empty() && (*arg)[0] != '0';
                } else if (k == 'i') {
                    cur.italic = !arg->empty() && (*arg)[0] != '0';
                } else if (k == 'H') {
                    bool relative = !arg->empty() && arg->back() == 'x';
                    std::string_view num = relative ? arg->substr(0, arg->size() - 1) : *arg;
                    double v = 0;
                    auto [p, ec] = std::from_chars(num.data(), num.data() + num.size(), v);
                    if (ec == std::errc{} && p == num.data() + num.size() && v > 0 &&
                        std::isfinite(v))
                        cur.height = relative ? cur.height * v : v;
                } else {
                    int v = 0;
                    auto [p, ec] = std::from_chars(arg->data(), arg->data() + arg->size(), v);
                    if (ec == std::errc{} && p == arg->data() + arg->size() && v >= 1 && v <= 255)
                        cur.color = v;
                }
                i = j;
            } else {
                text += c;
                ++i;
            }
        } else {
            text += c;
            ++i;
        }
    }
    flush();
    return out;
}

std::string plainText(std::string_view markup) {
    std::string out;
    for (const Piece& p : parseMarkup(markup, 1.0))
        out += p.newline ? std::string("\n") : p.text;
    return out;
}

std::string markupFromPlain(std::string_view plain) {
    std::string out;
    for (std::size_t i = 0; i < plain.size(); ++i) {
        char c = plain[i];
        if (c == '\r') {
            if (i + 1 < plain.size() && plain[i + 1] == '\n')
                ++i;
            out += "\\P";
        } else if (c == '\n') {
            out += "\\P";
        } else if (c == '\\' || c == '{' || c == '}') {
            out += '\\';
            out += c;
        } else {
            out += c;
        }
    }
    return out;
}

}  // namespace tbcad::text
