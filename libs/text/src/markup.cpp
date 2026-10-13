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
        } else if (c == '\\' && i + 1 < s.size()) {
            char k = s[i + 1];
            if (k == '\\' || k == '{' || k == '}') {
                text += k;
                i += 2;
            } else if (k == 'P') {
                flush();
                Piece p;
                p.newline = true;
                p.attr = cur;
                out.push_back(std::move(p));
                i += 2;
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
