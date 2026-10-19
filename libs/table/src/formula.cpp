// Table cell formulas: a cell whose text starts with "=" is worked out when it is drawn.
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <set>
#include <string>
#include <unordered_map>

#include "tbcad/table/table.h"

namespace tbcad::table {

namespace {

constexpr int kMaxDepth = 64;

struct Eval {
    const TableData& t;
    std::set<std::size_t> active{};  // cells being worked out, to catch a formula that needs itself
    // Formula cells already worked out in this evaluation: a cell used by two others (=A1+A1 down
    // a column) is worked out once, not once per way of reaching it.
    std::unordered_map<std::size_t, double> done{};
    int depth = 0;
    std::string error{};
};

struct Parser {
    Eval& ev;
    std::string_view s;
    std::size_t at = 0;
    bool failed = false;

    void fail(const char* what) {
        if (!failed) {
            failed = true;
            ev.error = what;
        }
    }
    void skip() {
        while (at < s.size() && std::isspace(static_cast<unsigned char>(s[at])))
            ++at;
    }
    bool eat(char c) {
        skip();
        if (at < s.size() && s[at] == c) {
            ++at;
            return true;
        }
        return false;
    }
    bool atEnd() {
        skip();
        return at >= s.size();
    }
};

double cellValue(Eval& ev, std::size_t row, std::size_t col, bool& numeric);

// "A1" -> (0,0); "AB12" -> (11, 27). False when the text is not a cell name.
bool parseName(std::string_view n, std::size_t& row, std::size_t& col) {
    std::size_t i = 0;
    std::size_t c = 0;
    while (i < n.size() && std::isalpha(static_cast<unsigned char>(n[i]))) {
        c = c * 26 +
            static_cast<std::size_t>(std::toupper(static_cast<unsigned char>(n[i])) - 'A' + 1);
        if (c > 100000)
            return false;
        ++i;
    }
    if (i == 0 || i == n.size())
        return false;
    std::size_t r = 0;
    for (; i < n.size(); ++i) {
        if (!std::isdigit(static_cast<unsigned char>(n[i])))
            return false;
        r = r * 10 + static_cast<std::size_t>(n[i] - '0');
        if (r > 1000000)
            return false;
    }
    if (r == 0)
        return false;
    row = r - 1;
    col = c - 1;
    return true;
}

double expression(Parser& p);

struct Range {
    std::size_t r0, c0, r1, c1;
};

// A cell name or a range A1:C3 at the current position.
bool readRef(Parser& p, Range& out, bool& isRange) {
    p.skip();
    std::size_t start = p.at;
    while (p.at < p.s.size() && std::isalnum(static_cast<unsigned char>(p.s[p.at])))
        ++p.at;
    std::size_t r0, c0;
    if (!parseName(p.s.substr(start, p.at - start), r0, c0)) {
        p.at = start;
        return false;
    }
    out = {r0, c0, r0, c0};
    isRange = false;
    if (p.at < p.s.size() && p.s[p.at] == ':') {
        const std::size_t save = p.at;
        ++p.at;
        const std::size_t s2 = p.at;
        while (p.at < p.s.size() && std::isalnum(static_cast<unsigned char>(p.s[p.at])))
            ++p.at;
        std::size_t r1, c1;
        if (!parseName(p.s.substr(s2, p.at - s2), r1, c1)) {
            p.at = save;
            return true;
        }
        out = {std::min(r0, r1), std::min(c0, c1), std::max(r0, r1), std::max(c0, c1)};
        isRange = true;
    }
    return true;
}

bool inside(const Eval& ev, const Range& r) {
    return r.r1 < rowCount(ev.t) && r.c1 < colCount(ev.t);
}

double functionCall(Parser& p, const std::string& name) {
    std::vector<double> vals;
    bool sawArg = false;
    if (!p.eat(')')) {
        for (;;) {
            sawArg = true;
            p.skip();
            const std::size_t save = p.at;
            Range r{};
            bool isRange = false;
            if (readRef(p, r, isRange) && isRange) {
                if (!inside(p.ev, r)) {
                    p.fail("#REF");
                    return 0;
                }
                for (std::size_t row = r.r0; row <= r.r1; ++row)
                    for (std::size_t col = r.c0; col <= r.c1; ++col) {
                        bool numeric = false;
                        const double v = cellValue(p.ev, row, col, numeric);
                        if (!p.ev.error.empty()) {
                            p.failed = true;
                            return 0;
                        }
                        if (numeric)
                            vals.push_back(v);
                    }
            } else {
                p.at = save;
                vals.push_back(expression(p));
            }
            if (p.failed)
                return 0;
            if (p.eat(','))
                continue;
            if (!p.eat(')')) {
                p.fail("#ERR");
                return 0;
            }
            break;
        }
    }
    (void)sawArg;
    auto upper = name;
    for (char& c : upper)
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    if (upper == "SUM") {
        double s = 0;
        for (double v : vals)
            s += v;
        return s;
    }
    if (upper == "COUNT")
        return static_cast<double>(vals.size());
    if (upper == "AVERAGE" || upper == "AVG") {
        if (vals.empty()) {
            p.fail("#DIV/0");
            return 0;
        }
        double s = 0;
        for (double v : vals)
            s += v;
        return s / static_cast<double>(vals.size());
    }
    if (upper == "MIN" || upper == "MAX") {
        if (vals.empty())
            return 0;
        return upper == "MIN" ? *std::min_element(vals.begin(), vals.end())
                              : *std::max_element(vals.begin(), vals.end());
    }
    if (upper == "ABS" && vals.size() == 1)
        return std::fabs(vals[0]);
    if (upper == "SQRT" && vals.size() == 1) {
        if (vals[0] < 0) {
            p.fail("#ERR");
            return 0;
        }
        return std::sqrt(vals[0]);
    }
    if (upper == "ROUND" && (vals.size() == 1 || vals.size() == 2)) {
        const double digits = vals.size() == 2 ? std::clamp(std::round(vals[1]), -12.0, 12.0) : 0;
        const double f = std::pow(10.0, digits);
        return std::round(vals[0] * f) / f;
    }
    p.fail("#NAME");
    return 0;
}

double primary(Parser& p) {
    if (p.failed)
        return 0;
    p.skip();
    if (p.at >= p.s.size()) {
        p.fail("#ERR");
        return 0;
    }
    const char c = p.s[p.at];
    if (c == '(') {
        ++p.at;
        const double v = expression(p);
        if (!p.failed && !p.eat(')'))
            p.fail("#ERR");
        return v;
    }
    if (c == '-') {
        ++p.at;
        return -primary(p);
    }
    if (c == '+') {
        ++p.at;
        return primary(p);
    }
    if (std::isdigit(static_cast<unsigned char>(c)) || c == '.') {
        const std::string rest(p.s.substr(p.at));
        char* end = nullptr;
        const double v = std::strtod(rest.c_str(), &end);
        if (end == rest.c_str()) {
            p.fail("#ERR");
            return 0;
        }
        p.at += static_cast<std::size_t>(end - rest.c_str());
        return v;
    }
    if (std::isalpha(static_cast<unsigned char>(c))) {
        // A function call name(...) or a cell name.
        std::size_t e = p.at;
        while (e < p.s.size() && std::isalnum(static_cast<unsigned char>(p.s[e])))
            ++e;
        if (e < p.s.size() && p.s[e] == '(') {
            const std::string name(p.s.substr(p.at, e - p.at));
            p.at = e + 1;
            return functionCall(p, name);
        }
        Range r{};
        bool isRange = false;
        if (readRef(p, r, isRange) && !isRange) {
            if (!inside(p.ev, r)) {
                p.fail("#REF");
                return 0;
            }
            bool numeric = false;
            const double v = cellValue(p.ev, r.r0, r.c0, numeric);
            if (!p.ev.error.empty()) {
                p.failed = true;
                return 0;
            }
            if (!numeric) {
                p.fail("#VALUE");
                return 0;
            }
            return v;
        }
        p.fail("#NAME");
        return 0;
    }
    p.fail("#ERR");
    return 0;
}

double power(Parser& p) {
    double v = primary(p);
    while (!p.failed && p.eat('^')) {
        const double e = primary(p);
        v = std::pow(v, e);
    }
    return v;
}

double term(Parser& p) {
    double v = power(p);
    for (;;) {
        if (p.failed)
            return 0;
        if (p.eat('*')) {
            v *= power(p);
        } else if (p.eat('/')) {
            const double d = power(p);
            if (p.failed)
                return 0;
            if (d == 0) {
                p.fail("#DIV/0");
                return 0;
            }
            v /= d;
        } else {
            return v;
        }
    }
}

double expression(Parser& p) {
    double v = term(p);
    for (;;) {
        if (p.failed)
            return 0;
        if (p.eat('+'))
            v += term(p);
        else if (p.eat('-'))
            v -= term(p);
        else
            return v;
    }
}

bool numberText(const std::string& text, double& out) {
    std::size_t b = 0, e = text.size();
    while (b < e && std::isspace(static_cast<unsigned char>(text[b])))
        ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(text[e - 1])))
        --e;
    if (b == e)
        return false;
    const std::string s = text.substr(b, e - b);
    char* end = nullptr;
    const double v = std::strtod(s.c_str(), &end);
    if (end != s.c_str() + s.size() || !std::isfinite(v))
        return false;
    out = v;
    return true;
}

// The value of a cell: a formula is worked out, text that reads as a number is that number, an
// empty cell is 0 (numeric = false so ranges leave it out), other text is not numeric.
double cellValue(Eval& ev, std::size_t row, std::size_t col, bool& numeric) {
    numeric = false;
    const CellRef a = anchorOf(ev.t, row, col);
    const TableCell& cell = cellAt(ev.t, a.row, a.col);
    const std::string body = cellBody(cell.utf8);
    if (body.empty())
        return 0;
    if (body[0] == '=') {
        const std::size_t key = a.row * colCount(ev.t) + a.col;
        if (const auto hit = ev.done.find(key); hit != ev.done.end()) {
            numeric = true;
            return hit->second;
        }
        if (ev.active.count(key) || ev.depth >= kMaxDepth) {
            ev.error = "#LOOP";
            return 0;
        }
        ev.active.insert(key);
        ++ev.depth;
        Parser p{ev, std::string_view(body).substr(1)};
        const double v = expression(p);
        if (!p.failed && !p.atEnd())
            p.fail("#ERR");
        --ev.depth;
        ev.active.erase(key);
        if (p.failed)
            return 0;
        numeric = true;
        ev.done[key] = v;
        return v;
    }
    double v = 0;
    if (numberText(body, v)) {
        numeric = true;
        return v;
    }
    return 0;
}

std::string formatNumber(double v) {
    if (!std::isfinite(v))
        return "#ERR";
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.6f", v);
    std::string s = buf;
    if (s.find('.') != std::string::npos) {
        while (s.back() == '0')
            s.pop_back();
        if (s.back() == '.')
            s.pop_back();
    }
    return s == "-0" ? std::string("0") : s;
}

}  // namespace

bool isFormula(const std::string& text) {
    return !text.empty() && text[0] == '=';
}

namespace {
constexpr std::string_view kAlignCodes[] = {"\\pxql;", "\\pxqc;", "\\pxqr;"};
}

std::optional<CellAlign> cellAlign(const TableCell& cell) {
    for (std::size_t i = 0; i < 3; ++i)
        if (cell.utf8.compare(0, kAlignCodes[i].size(), kAlignCodes[i]) == 0)
            return static_cast<CellAlign>(i);
    return std::nullopt;
}

std::string cellBody(const std::string& utf8) {
    for (std::string_view code : kAlignCodes)
        if (utf8.compare(0, code.size(), code) == 0)
            return utf8.substr(code.size());
    return utf8;
}

bool setCellAlign(TableData& t, std::size_t row, std::size_t col, std::optional<CellAlign> align) {
    if (row >= rowCount(t) || col >= colCount(t))
        return false;
    TableCell& cell = cellAt(t, row, col);
    if (cell.covered)
        return false;
    std::string body = cellBody(cell.utf8);
    cell.utf8 = align ? std::string(kAlignCodes[static_cast<std::size_t>(*align)]) + body : body;
    return true;
}

std::string cellName(std::size_t row, std::size_t col) {
    std::string letters;
    for (std::size_t c = col + 1; c > 0; c = (c - 1) / 26)
        letters.insert(letters.begin(), static_cast<char>('A' + (c - 1) % 26));
    return letters + std::to_string(row + 1);
}

std::string displayText(const TableData& t, std::size_t row, std::size_t col) {
    const TableCell& cell = cellAt(t, row, col);
    const std::string body = cellBody(cell.utf8);
    if (!isFormula(body))
        return body;
    Eval ev{t};
    bool numeric = false;
    const double v = cellValue(ev, row, col, numeric);
    if (!ev.error.empty())
        return ev.error;
    return formatNumber(v);
}

}  // namespace tbcad::table
