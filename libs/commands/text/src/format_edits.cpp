#include "tbcad/textcmd/format_edits.h"

#include <algorithm>
#include <cctype>
#include <cstdio>

namespace tbcad::textcmd {

namespace {

constexpr std::string_view kBullet = "\xE2\x80\xA2";  // U+2022

std::string num(double v) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.6g", v);
    return buf;
}

std::size_t paraStart(const std::string& t, std::size_t pos) {
    pos = std::min(pos, t.size());
    const std::size_t nl = pos == 0 ? std::string::npos : t.rfind('\n', pos - 1);
    return nl == std::string::npos ? 0 : nl + 1;
}

std::size_t paraEnd(const std::string& t, std::size_t pos) {
    const std::size_t nl = t.find('\n', std::min(pos, t.size()));
    return nl == std::string::npos ? t.size() : nl;
}

// Length of a "\p...;" code at the start of `line`, 0 when there is none.
std::size_t leadingParaCode(std::string_view line) {
    if (line.size() < 3 || line[0] != '\\' || line[1] != 'p')
        return 0;
    const std::size_t semi = line.find(';');
    return semi == std::string_view::npos ? 0 : semi + 1;
}

// Length of a list mark plus its tab ("•\t", "12.\t", "b.\t") at the start of `line`, or 0.
std::size_t leadingMark(std::string_view line) {
    if (line.substr(0, kBullet.size()) == kBullet && line.size() > kBullet.size() &&
        line[kBullet.size()] == '\t')
        return kBullet.size() + 1;
    std::size_t i = 0;
    while (i < line.size() && std::isdigit(static_cast<unsigned char>(line[i])))
        ++i;
    if (i == 0 && !line.empty() && std::islower(static_cast<unsigned char>(line[0])))
        i = 1;
    if (i > 0 && i + 1 < line.size() && line[i] == '.' && line[i + 1] == '\t')
        return i + 2;
    return 0;
}

std::string mark(ListKind kind, std::size_t n) {
    switch (kind) {
        case ListKind::Bullets:
            return std::string(kBullet) + "\t";
        case ListKind::Numbers:
            return std::to_string(n + 1) + ".\t";
        case ListKind::Letters: {
            std::string s;
            for (std::size_t k = n;; k = k / 26 - 1) {
                s.insert(s.begin(), static_cast<char>('a' + k % 26));
                if (k < 26)
                    break;
            }
            return s + ".\t";
        }
        case ListKind::Off:
            break;
    }
    return {};
}

}  // namespace

TextSel wrapSelection(TextSel s, std::string_view open, std::string_view close) {
    const std::size_t a = std::min({s.start, s.end, s.text.size()});
    const std::size_t b = std::min(std::max(s.start, s.end), s.text.size());
    s.text.insert(b, close);
    s.text.insert(a, open);
    s.start = a + open.size();
    s.end = b + open.size();
    return s;
}

TextSel insertText(TextSel s, std::string_view text) {
    const std::size_t a = std::min({s.start, s.end, s.text.size()});
    const std::size_t b = std::min(std::max(s.start, s.end), s.text.size());
    s.text.replace(a, b - a, text);
    s.start = s.end = a + text.size();
    return s;
}

bool stackSelection(TextSel& s) {
    const std::size_t a = std::min({s.start, s.end, s.text.size()});
    const std::size_t b = std::min(std::max(s.start, s.end), s.text.size());
    const std::string sel = s.text.substr(a, b - a);
    if (sel.find_first_of("/#^") == std::string::npos ||
        sel.find_first_of(";\n\\") != std::string::npos)
        return false;
    s = insertText(s, "\\S" + sel + ";");
    return true;
}

TextSel makeList(TextSel s, ListKind kind, double indent) {
    std::string& t = s.text;
    const std::size_t lo = std::min({s.start, s.end, t.size()});
    std::size_t hi = std::min(std::max(s.start, s.end), t.size());
    if (hi > lo && t[hi - 1] == '\n')
        --hi;  // a selection ending at the start of a line does not take that line
    const std::size_t first = paraStart(t, lo);
    const std::size_t last = paraEnd(t, hi);
    std::vector<std::string> lines;
    for (std::size_t p = first;;) {
        const std::size_t e = paraEnd(t, p);
        lines.push_back(t.substr(p, e - p));
        if (e >= last)
            break;
        p = e + 1;
    }
    const std::string code = "\\pxi-" + num(indent) + ",l" + num(indent) + ",t" + num(indent) + ";";
    std::string block;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        std::string_view line = lines[i];
        const std::size_t c = leadingParaCode(line);
        const std::string_view oldCode = line.substr(0, c);
        line.remove_prefix(c);
        line.remove_prefix(leadingMark(line));
        std::string out;
        if (kind == ListKind::Off) {
            // Keep paragraph codes that are not a list's hanging indent.
            if (!oldCode.empty() && oldCode.substr(0, 5) != "\\pxi-")
                out += oldCode;
        } else {
            out += i == 0 ? code : std::string();
            out += mark(kind, i);
        }
        out += line;
        block += (i ? "\n" : "") + out;
    }
    std::string after = t.substr(last);
    // The paragraph after the list starts without the indent again.
    if (kind != ListKind::Off && after.size() > 1 && after[0] == '\n' &&
        leadingParaCode(std::string_view(after).substr(1)) == 0)
        after.insert(1, "\\pxi0,l0;");
    t = t.substr(0, first) + block + after;
    s.start = first;
    s.end = first + block.size();
    return s;
}

TextSel setParagraphCode(TextSel s, std::string_view code) {
    const std::size_t p = paraStart(s.text, std::min(s.start, s.end));
    const std::size_t old = leadingParaCode(std::string_view(s.text).substr(p));
    s.text.replace(p, old, code);
    const auto shift = [&](std::size_t v) {
        return v < p + old ? p + code.size() : v - old + code.size();
    };
    s.start = shift(s.start);
    s.end = shift(s.end);
    return s;
}

std::string paragraphCode(double firstIndent, double leftIndent, double rightIndent,
                          const std::vector<double>& tabs, double spaceBefore, double spaceAfter,
                          std::string_view align) {
    std::string items;
    auto add = [&](const std::string& item) { items += (items.empty() ? "" : ",") + item; };
    if (firstIndent != 0)
        add("i" + num(firstIndent));
    if (leftIndent != 0)
        add("l" + num(leftIndent));
    if (rightIndent != 0)
        add("r" + num(rightIndent));
    if (!align.empty())
        add("q" + std::string(align));
    if (!tabs.empty()) {
        std::string t = "t";
        for (std::size_t i = 0; i < tabs.size(); ++i)
            t += (i ? "," : "") + num(tabs[i]);
        add(t);
    }
    if (spaceBefore != 0)
        add("sb" + num(spaceBefore));
    if (spaceAfter != 0)
        add("sa" + num(spaceAfter));
    if (items.empty())
        return {};
    return "\\px" + items + ";";
}

std::string replaceAll(std::string_view text, std::string_view find, std::string_view with,
                       bool matchCase, std::size_t& count) {
    count = 0;
    if (find.empty())
        return std::string(text);
    auto same = [&](char a, char b) {
        return matchCase ? a == b
                         : std::tolower(static_cast<unsigned char>(a)) ==
                               std::tolower(static_cast<unsigned char>(b));
    };
    std::string out;
    std::size_t i = 0;
    while (i < text.size()) {
        if (i + find.size() <= text.size() &&
            std::equal(find.begin(), find.end(), text.begin() + static_cast<std::ptrdiff_t>(i),
                       same)) {
            out += with;
            i += find.size();
            ++count;
        } else {
            out += text[i++];
        }
    }
    return out;
}

}  // namespace tbcad::textcmd
