#include "tbcad/table/table.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <numeric>

namespace tbcad::table {

namespace {

constexpr std::size_t kMaxCols = 1000;
constexpr std::size_t kMaxRows = 100000;

std::size_t idx(const TableData& t, std::size_t r, std::size_t c) {
    return r * t.colWidths.size() + c;
}

double sum(const std::vector<double>& v, std::size_t from, std::size_t to) {
    return std::accumulate(v.begin() + static_cast<std::ptrdiff_t>(from),
                           v.begin() + static_cast<std::ptrdiff_t>(to), 0.0);
}

Vec2 rotate(Vec2 v, double a) {
    const double c = std::cos(a), s = std::sin(a);
    return {v.x * c - v.y * s, v.x * s + v.y * c};
}

bool valid(const TableData& t) {
    return !t.colWidths.empty() && !t.rowHeights.empty() &&
           t.cells.size() == t.colWidths.size() * t.rowHeights.size();
}

// Splits every merged range for which `pred(anchorRow, anchorCol, cell)` holds.
template <class Pred>
void unmergeWhere(TableData& t, Pred pred) {
    const std::size_t rows = rowCount(t), cols = colCount(t);
    for (std::size_t r = 0; r < rows; ++r) {
        for (std::size_t c = 0; c < cols; ++c) {
            TableCell& a = t.cells[idx(t, r, c)];
            if (a.covered || (a.colSpan == 1 && a.rowSpan == 1) || !pred(r, c, a))
                continue;
            for (std::size_t rr = r; rr < r + a.rowSpan; ++rr) {
                for (std::size_t cc = c; cc < c + a.colSpan; ++cc)
                    t.cells[idx(t, rr, cc)].covered = false;
            }
            a.colSpan = a.rowSpan = 1;
        }
    }
}

}  // namespace

TableCell& cellAt(TableData& t, std::size_t row, std::size_t col) {
    return t.cells[idx(t, row, col)];
}

const TableCell& cellAt(const TableData& t, std::size_t row, std::size_t col) {
    return t.cells[idx(t, row, col)];
}

double totalWidth(const TableData& t) {
    return sum(t.colWidths, 0, t.colWidths.size());
}

double totalHeight(const TableData& t) {
    return sum(t.rowHeights, 0, t.rowHeights.size());
}

Vec2 toWorld(const TableData& t, Vec2 local) {
    return t.origin + rotate({local.x, -local.y}, t.rotation);
}

Vec2 toLocal(const TableData& t, Vec2 world) {
    Vec2 d = rotate(world - t.origin, -t.rotation);
    return {d.x, -d.y};
}

CellRef anchorOf(const TableData& t, std::size_t row, std::size_t col) {
    if (!valid(t) || row >= rowCount(t) || col >= colCount(t))
        return {row, col};
    if (!cellAt(t, row, col).covered)
        return {row, col};
    for (std::size_t r = 0; r <= row; ++r) {
        for (std::size_t c = 0; c <= col; ++c) {
            const TableCell& a = cellAt(t, r, c);
            if (!a.covered && r + a.rowSpan > row && c + a.colSpan > col)
                return {r, c};
        }
    }
    return {row, col};
}

Rect cellRect(const TableData& t, std::size_t row, std::size_t col) {
    CellRef a = anchorOf(t, row, col);
    const TableCell& c = cellAt(t, a.row, a.col);
    Rect r;
    r.x0 = sum(t.colWidths, 0, a.col);
    r.x1 = r.x0 + sum(t.colWidths, a.col, a.col + c.colSpan);
    r.y0 = sum(t.rowHeights, 0, a.row);
    r.y1 = r.y0 + sum(t.rowHeights, a.row, a.row + c.rowSpan);
    return r;
}

TableData make(std::size_t rows, std::size_t cols, double colWidth, double rowHeight,
               const std::string& title, Vec2 origin, double textHeight) {
    TableData t;
    rows = std::clamp<std::size_t>(rows, 1, kMaxRows);
    cols = std::clamp<std::size_t>(cols, 1, kMaxCols);
    t.origin = origin;
    t.textHeight = textHeight > 0 ? textHeight : 2.5;
    t.margin = t.textHeight * 0.4;
    t.colWidths.assign(cols, colWidth > 0 ? colWidth : 30.0);
    t.rowHeights.assign(rows, rowHeight > 0 ? rowHeight : t.textHeight * 2.0);
    t.cells.assign(rows * cols, TableCell{});
    if (!title.empty()) {
        t.titleRow = true;
        TableCell& c = t.cells[0];
        c.utf8 = title;
        c.colSpan = static_cast<std::uint16_t>(cols);
        for (std::size_t i = 1; i < cols; ++i)
            t.cells[i].covered = true;
    }
    return t;
}

std::optional<CellRef> hit(const TableData& t, Vec2 world) {
    if (!valid(t))
        return std::nullopt;
    const Vec2 p = toLocal(t, world);
    if (p.x < 0 || p.y < 0 || p.x > totalWidth(t) || p.y > totalHeight(t))
        return std::nullopt;
    auto find = [](const std::vector<double>& v, double x) {
        double acc = 0;
        for (std::size_t i = 0; i < v.size(); ++i) {
            acc += v[i];
            if (x <= acc)
                return i;
        }
        return v.size() - 1;
    };
    return anchorOf(t, find(t.rowHeights, p.y), find(t.colWidths, p.x));
}

bool setText(TableData& t, std::size_t row, std::size_t col, const std::string& utf8) {
    if (!valid(t) || row >= rowCount(t) || col >= colCount(t))
        return false;
    CellRef a = anchorOf(t, row, col);
    cellAt(t, a.row, a.col).utf8 = utf8;
    return true;
}

bool insertRow(TableData& t, std::size_t at, double height) {
    if (!valid(t) || at > rowCount(t) || rowCount(t) >= kMaxRows)
        return false;
    if (t.titleRow && at == 0)
        at = 1;
    unmergeWhere(t, [&](std::size_t r, std::size_t, const TableCell& a) {
        return r < at && at < r + a.rowSpan;
    });
    if (!(height > 0))
        height = t.rowHeights[at < rowCount(t) ? at : at - 1];
    const std::size_t cols = colCount(t);
    t.cells.insert(t.cells.begin() + static_cast<std::ptrdiff_t>(at * cols), cols, TableCell{});
    t.rowHeights.insert(t.rowHeights.begin() + static_cast<std::ptrdiff_t>(at), height);
    return true;
}

bool deleteRow(TableData& t, std::size_t at) {
    if (!valid(t) || at >= rowCount(t) || rowCount(t) <= 1)
        return false;
    unmergeWhere(t, [&](std::size_t r, std::size_t, const TableCell& a) {
        return r <= at && at < r + a.rowSpan;
    });
    const std::size_t cols = colCount(t);
    auto first = t.cells.begin() + static_cast<std::ptrdiff_t>(at * cols);
    t.cells.erase(first, first + static_cast<std::ptrdiff_t>(cols));
    t.rowHeights.erase(t.rowHeights.begin() + static_cast<std::ptrdiff_t>(at));
    if (at == 0)
        t.titleRow = false;
    return true;
}

bool insertCol(TableData& t, std::size_t at, double width) {
    if (!valid(t) || at > colCount(t) || colCount(t) >= kMaxCols)
        return false;
    unmergeWhere(t, [&](std::size_t, std::size_t c, const TableCell& a) {
        return c < at && at < c + a.colSpan;
    });
    if (!(width > 0))
        width = t.colWidths[at < colCount(t) ? at : at - 1];
    const std::size_t rows = rowCount(t), cols = colCount(t);
    std::vector<TableCell> next;
    next.reserve(t.cells.size() + rows);
    for (std::size_t r = 0; r < rows; ++r) {
        for (std::size_t c = 0; c <= cols; ++c) {
            if (c == at)
                next.emplace_back();
            if (c < cols)
                next.push_back(std::move(t.cells[r * cols + c]));
        }
    }
    t.cells = std::move(next);
    t.colWidths.insert(t.colWidths.begin() + static_cast<std::ptrdiff_t>(at), width);
    return true;
}

bool deleteCol(TableData& t, std::size_t at) {
    if (!valid(t) || at >= colCount(t) || colCount(t) <= 1)
        return false;
    unmergeWhere(t, [&](std::size_t, std::size_t c, const TableCell& a) {
        return c <= at && at < c + a.colSpan;
    });
    const std::size_t rows = rowCount(t), cols = colCount(t);
    std::vector<TableCell> next;
    next.reserve(t.cells.size() - rows);
    for (std::size_t r = 0; r < rows; ++r) {
        for (std::size_t c = 0; c < cols; ++c) {
            if (c != at)
                next.push_back(std::move(t.cells[r * cols + c]));
        }
    }
    t.cells = std::move(next);
    t.colWidths.erase(t.colWidths.begin() + static_cast<std::ptrdiff_t>(at));
    return true;
}

bool merge(TableData& t, CellRef a, CellRef b) {
    if (!valid(t) || a.row >= rowCount(t) || b.row >= rowCount(t) || a.col >= colCount(t) ||
        b.col >= colCount(t))
        return false;
    const std::size_t r0 = std::min(a.row, b.row), r1 = std::max(a.row, b.row);
    const std::size_t c0 = std::min(a.col, b.col), c1 = std::max(a.col, b.col);
    if (r0 == r1 && c0 == c1)
        return false;
    // Every merged range that touches the area must lie fully inside it.
    for (std::size_t r = 0; r < rowCount(t); ++r) {
        for (std::size_t c = 0; c < colCount(t); ++c) {
            const TableCell& m = cellAt(t, r, c);
            if (m.covered || (m.colSpan == 1 && m.rowSpan == 1))
                continue;
            const bool touches = r <= r1 && r + m.rowSpan > r0 && c <= c1 && c + m.colSpan > c0;
            const bool inside =
                r >= r0 && r + m.rowSpan - 1 <= r1 && c >= c0 && c + m.colSpan - 1 <= c1;
            if (touches && !inside)
                return false;
        }
    }
    std::string text;
    for (std::size_t r = r0; r <= r1; ++r) {
        for (std::size_t c = c0; c <= c1; ++c) {
            TableCell& cell = cellAt(t, r, c);
            if (!cell.covered && !cell.utf8.empty()) {
                if (!text.empty())
                    text += ' ';
                text += cell.utf8;
            }
            cell.utf8.clear();
            cell.colSpan = cell.rowSpan = 1;
            cell.covered = true;
        }
    }
    TableCell& anchor = cellAt(t, r0, c0);
    anchor.covered = false;
    anchor.utf8 = std::move(text);
    anchor.colSpan = static_cast<std::uint16_t>(c1 - c0 + 1);
    anchor.rowSpan = static_cast<std::uint16_t>(r1 - r0 + 1);
    return true;
}

bool unmerge(TableData& t, std::size_t row, std::size_t col) {
    if (!valid(t) || row >= rowCount(t) || col >= colCount(t))
        return false;
    CellRef a = anchorOf(t, row, col);
    const TableCell m = cellAt(t, a.row, a.col);
    if (m.colSpan == 1 && m.rowSpan == 1)
        return false;
    for (std::size_t r = a.row; r < a.row + m.rowSpan; ++r) {
        for (std::size_t c = a.col; c < a.col + m.colSpan; ++c)
            cellAt(t, r, c).covered = false;
    }
    TableCell& anchor = cellAt(t, a.row, a.col);
    anchor.colSpan = anchor.rowSpan = 1;
    return true;
}

bool setColumnWidth(TableData& t, std::size_t col, double width) {
    if (col >= colCount(t) || !(width > 0))
        return false;
    t.colWidths[col] = width;
    return true;
}

bool setRowHeight(TableData& t, std::size_t row, double height) {
    if (row >= rowCount(t) || !(height > 0))
        return false;
    t.rowHeights[row] = height;
    return true;
}

Layout layout(const TableData& t) {
    Layout out;
    if (!valid(t))
        return out;
    const std::size_t rows = rowCount(t), cols = colCount(t);
    std::vector<double> xs(cols + 1, 0.0), ys(rows + 1, 0.0);
    for (std::size_t c = 0; c < cols; ++c)
        xs[c + 1] = xs[c] + t.colWidths[c];
    for (std::size_t r = 0; r < rows; ++r)
        ys[r + 1] = ys[r] + t.rowHeights[r];
    auto same = [&](std::size_t r0, std::size_t c0, std::size_t r1, std::size_t c1) {
        return anchorOf(t, r0, c0) == anchorOf(t, r1, c1);
    };
    auto seg = [&](Vec2 a, Vec2 b) { out.lines.emplace_back(toWorld(t, a), toWorld(t, b)); };

    // Horizontal lines: boundary i between row i-1 and row i, drawn per column unless inside a
    // merge.
    for (std::size_t i = 0; i <= rows; ++i) {
        std::size_t start = 0;
        bool open = false;
        for (std::size_t c = 0; c <= cols; ++c) {
            const bool draw = c < cols && (i == 0 || i == rows || !same(i - 1, c, i, c));
            if (draw && !open) {
                start = c;
                open = true;
            } else if (!draw && open) {
                seg({xs[start], ys[i]}, {xs[c], ys[i]});
                open = false;
            }
        }
    }
    for (std::size_t j = 0; j <= cols; ++j) {
        std::size_t start = 0;
        bool open = false;
        for (std::size_t r = 0; r <= rows; ++r) {
            const bool draw = r < rows && (j == 0 || j == cols || !same(r, j - 1, r, j));
            if (draw && !open) {
                start = r;
                open = true;
            } else if (!draw && open) {
                seg({xs[j], ys[start]}, {xs[j], ys[r]});
                open = false;
            }
        }
    }

    for (std::size_t r = 0; r < rows; ++r) {
        for (std::size_t c = 0; c < cols; ++c) {
            const TableCell& cell = cellAt(t, r, c);
            if (cell.covered || cellBody(cell.utf8).empty())
                continue;
            const Rect rc = cellRect(t, r, c);
            TextItem item;
            item.rotation = t.rotation;
            item.utf8 = displayText(t, r, c);
            item.width = std::max(0.0, rc.x1 - rc.x0 - 2 * t.margin);
            const double cy = (rc.y0 + rc.y1) / 2;
            const std::optional<CellAlign> align = cellAlign(cell);
            const CellAlign eff =
                align ? *align : (t.titleRow && r == 0 ? CellAlign::Center : CellAlign::Left);
            if (eff == CellAlign::Center) {
                item.attach = Attachment::MiddleCenter;
                item.position = toWorld(t, {(rc.x0 + rc.x1) / 2, cy});
            } else if (eff == CellAlign::Right) {
                item.attach = Attachment::MiddleRight;
                item.position = toWorld(t, {rc.x1 - t.margin, cy});
            } else {
                item.attach = Attachment::MiddleLeft;
                item.position = toWorld(t, {rc.x0 + t.margin, cy});
            }
            out.texts.push_back(std::move(item));
        }
    }
    return out;
}

BBox2 bounds(const TableData& t) {
    BBox2 b;
    if (!valid(t))
        return b;
    const double w = totalWidth(t), h = totalHeight(t);
    for (Vec2 p : {Vec2{0, 0}, Vec2{w, 0}, Vec2{w, h}, Vec2{0, h}})
        b.expand(toWorld(t, p));
    return b;
}

// ---- text form --------------------------------------------------------------------------------

namespace {

std::string escape(const std::string& in) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : in) {
        if (std::isalnum(c) || c == ' ' || c == '.' || c == ',' || c == '_' || c == '-') {
            out += static_cast<char>(c);
        } else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 15];
        }
    }
    return out;
}

bool unescape(std::string_view in, std::string& out) {
    out.clear();
    for (std::size_t i = 0; i < in.size(); ++i) {
        if (in[i] != '%') {
            out += in[i];
            continue;
        }
        if (i + 2 >= in.size())
            return false;
        auto val = [](char c) {
            if (c >= '0' && c <= '9')
                return c - '0';
            if (c >= 'A' && c <= 'F')
                return c - 'A' + 10;
            return -1;
        };
        int h = val(in[i + 1]), l = val(in[i + 2]);
        if (h < 0 || l < 0)
            return false;
        out += static_cast<char>(h * 16 + l);
        i += 2;
    }
    return true;
}

std::vector<std::string_view> split(std::string_view s, char sep) {
    std::vector<std::string_view> out;
    std::size_t at = 0;
    for (;;) {
        std::size_t n = s.find(sep, at);
        if (n == std::string_view::npos) {
            out.push_back(s.substr(at));
            return out;
        }
        out.push_back(s.substr(at, n - at));
        at = n + 1;
    }
}

std::string num(double v) {
    char buf[40];
    std::snprintf(buf, sizeof buf, "%.17g", v);
    return buf;
}

bool toDouble(std::string_view s, double& v) {
    std::string tmp(s);
    char* end = nullptr;
    v = std::strtod(tmp.c_str(), &end);
    return !tmp.empty() && end == tmp.c_str() + tmp.size() && std::isfinite(v);
}

bool toUnsigned(std::string_view s, std::size_t& v) {
    if (s.empty() || s.size() > 9)
        return false;
    v = 0;
    for (char c : s) {
        if (c < '0' || c > '9')
            return false;
        v = v * 10 + static_cast<std::size_t>(c - '0');
    }
    return true;
}

}  // namespace

std::string serialize(const TableData& t) {
    std::string s = "tbcad-table 1|" + num(t.origin.x) + "|" + num(t.origin.y) + "|" +
                    num(t.rotation) + "|" + num(t.textHeight) + "|" + num(t.margin) + "|" +
                    (t.titleRow ? "1" : "0") + "|";
    for (std::size_t i = 0; i < t.colWidths.size(); ++i)
        s += (i ? "," : "") + num(t.colWidths[i]);
    s += "|";
    for (std::size_t i = 0; i < t.rowHeights.size(); ++i)
        s += (i ? "," : "") + num(t.rowHeights[i]);
    s += "|";
    for (std::size_t i = 0; i < t.cells.size(); ++i) {
        const TableCell& c = t.cells[i];
        s += (i ? ";" : "") + std::to_string(c.colSpan) + ":" + std::to_string(c.rowSpan) + ":" +
             (c.covered ? "1" : "0") + ":" + escape(c.utf8);
    }
    return s;
}

std::optional<TableData> parse(std::string_view s) {
    auto parts = split(s, '|');
    if (parts.size() != 10 || parts[0] != "tbcad-table 1")
        return std::nullopt;
    TableData t;
    if (!toDouble(parts[1], t.origin.x) || !toDouble(parts[2], t.origin.y) ||
        !toDouble(parts[3], t.rotation) || !toDouble(parts[4], t.textHeight) ||
        !toDouble(parts[5], t.margin))
        return std::nullopt;
    t.titleRow = parts[6] == "1";
    for (std::string_view w : split(parts[7], ',')) {
        double v = 0;
        if (!toDouble(w, v) || !(v > 0) || t.colWidths.size() >= kMaxCols)
            return std::nullopt;
        t.colWidths.push_back(v);
    }
    for (std::string_view h : split(parts[8], ',')) {
        double v = 0;
        if (!toDouble(h, v) || !(v > 0) || t.rowHeights.size() >= kMaxRows)
            return std::nullopt;
        t.rowHeights.push_back(v);
    }
    auto cells = split(parts[9], ';');
    if (cells.size() != t.colWidths.size() * t.rowHeights.size() || !(t.textHeight > 0))
        return std::nullopt;
    const std::size_t cols = t.colWidths.size(), rows = t.rowHeights.size();
    for (std::size_t i = 0; i < cells.size(); ++i) {
        auto f = split(cells[i], ':');
        std::size_t cs = 0, rs = 0;
        if (f.size() != 4 || !toUnsigned(f[0], cs) || !toUnsigned(f[1], rs) || cs < 1 || rs < 1 ||
            i % cols + cs > cols || i / cols + rs > rows)
            return std::nullopt;
        TableCell c;
        c.colSpan = static_cast<std::uint16_t>(cs);
        c.rowSpan = static_cast<std::uint16_t>(rs);
        c.covered = f[2] == "1";
        if (!unescape(f[3], c.utf8))
            return std::nullopt;
        t.cells.push_back(std::move(c));
    }
    return t;
}

}  // namespace tbcad::table
