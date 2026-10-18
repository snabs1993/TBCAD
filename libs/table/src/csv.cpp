#include <algorithm>

#include "tbcad/table/table.h"
#include "tbcad/text/text.h"

namespace tbcad::table {

namespace {

std::string quoteField(const std::string& s) {
    if (s.find_first_of(",\"\r\n") == std::string::npos)
        return s;
    std::string q = "\"";
    for (char c : s) {
        if (c == '"')
            q += '"';
        q += c;
    }
    return q + "\"";
}

}  // namespace

std::string toCsv(const TableData& t) {
    std::string out;
    const std::size_t rows = rowCount(t), cols = colCount(t);
    for (std::size_t r = 0; r < rows; ++r) {
        for (std::size_t c = 0; c < cols; ++c) {
            const TableCell& cell = cellAt(t, r, c);
            if (c)
                out += ',';
            if (!cell.covered)
                out += quoteField(text::plainText(cell.utf8));
        }
        out += "\r\n";
    }
    return out;
}

std::vector<std::vector<std::string>> parseCsv(std::string_view csv) {
    std::vector<std::vector<std::string>> rows;
    if (csv.size() >= 3 && csv.substr(0, 3) == "\xEF\xBB\xBF")
        csv.remove_prefix(3);
    // The separator is the comma, or the semicolon when the first line has more of those.
    char sep = ',';
    {
        std::size_t commas = 0, semis = 0;
        bool q = false;
        for (char c : csv) {
            if (c == '"')
                q = !q;
            else if (!q && (c == '\n' || c == '\r'))
                break;
            else if (!q && c == ',')
                ++commas;
            else if (!q && c == ';')
                ++semis;
        }
        if (semis > commas)
            sep = ';';
    }
    std::vector<std::string> row;
    std::string field;
    bool quoted = false, any = false;
    auto endField = [&] {
        row.push_back(std::move(field));
        field.clear();
        any = true;
    };
    auto endRow = [&] {
        if (any || !field.empty()) {
            endField();
            rows.push_back(std::move(row));
        }
        row.clear();
        any = false;
    };
    for (std::size_t i = 0; i < csv.size(); ++i) {
        const char c = csv[i];
        if (quoted) {
            if (c == '"') {
                if (i + 1 < csv.size() && csv[i + 1] == '"') {
                    field += '"';
                    ++i;
                } else {
                    quoted = false;
                }
            } else {
                field += c;
            }
        } else if (c == '"' && field.empty()) {
            quoted = true;
            any = true;
        } else if (c == sep) {
            endField();
        } else if (c == '\r') {
            if (i + 1 < csv.size() && csv[i + 1] == '\n')
                ++i;
            endRow();
        } else if (c == '\n') {
            endRow();
        } else {
            field += c;
        }
    }
    endRow();
    return rows;
}

TableData fromRows(const std::vector<std::vector<std::string>>& rows, Vec2 origin,
                   double textHeight, const TextStyle* style) {
    std::size_t cols = 1;
    for (const auto& r : rows)
        cols = std::max(cols, r.size());
    TableData t = make(std::max<std::size_t>(rows.size(), 1), cols, 30.0, textHeight * 2.0, "",
                       origin, textHeight);
    for (std::size_t c = 0; c < cols; ++c) {
        double w = textHeight * 4;
        for (const auto& r : rows)
            if (c < r.size())
                w = std::max(w, text::measureWidth(style, r[c], textHeight) + 2 * t.margin);
        t.colWidths[c] = w;
    }
    for (std::size_t r = 0; r < rows.size(); ++r)
        for (std::size_t c = 0; c < rows[r].size(); ++c)
            cellAt(t, r, c).utf8 = text::markupFromPlain(rows[r][c]);
    return t;
}

}  // namespace tbcad::table
