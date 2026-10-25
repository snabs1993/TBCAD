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

void appendUtf8(std::string& out, char32_t cp) {
    if (cp < 0x80) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

bool validUtf8(std::string_view s) {
    for (std::size_t i = 0; i < s.size();) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        const std::size_t n = c < 0x80         ? 1
                              : (c >> 5) == 6  ? 2
                              : (c >> 4) == 14 ? 3
                              : (c >> 3) == 30 ? 4
                                               : 0;
        if (n == 0 || i + n > s.size())
            return false;
        for (std::size_t k = 1; k < n; ++k)
            if ((static_cast<unsigned char>(s[i + k]) & 0xC0) != 0x80)
                return false;
        i += n;
    }
    return true;
}

// Excel's "Unicode Text" is UTF-16 with a byte order mark.
std::string fromUtf16(std::string_view s, bool bigEndian) {
    std::string out;
    for (std::size_t i = 2; i + 1 < s.size(); i += 2) {
        const auto b0 = static_cast<unsigned char>(s[i]), b1 = static_cast<unsigned char>(s[i + 1]);
        char32_t u = bigEndian ? (b0 << 8 | b1) : (b1 << 8 | b0);
        if (u >= 0xD800 && u < 0xDC00 && i + 3 < s.size()) {
            const auto c0 = static_cast<unsigned char>(s[i + 2]),
                       c1 = static_cast<unsigned char>(s[i + 3]);
            const char32_t lo = bigEndian ? (c0 << 8 | c1) : (c1 << 8 | c0);
            if (lo >= 0xDC00 && lo < 0xE000) {
                u = 0x10000 + ((u - 0xD800) << 10) + (lo - 0xDC00);
                i += 2;
            }
        }
        appendUtf8(out, u);
    }
    return out;
}

// A CSV saved by Excel on a Thai Windows PC ("CSV (Comma delimited)") is in the ANSI code page
// 874, any other legacy one in Windows-1252.
std::string fromLegacy(std::string_view s) {
    std::size_t thai = 0, high = 0;
    for (char ch : s) {
        const auto c = static_cast<unsigned char>(ch);
        if (c >= 0x80) {
            ++high;
            thai += (c >= 0xA1 && c <= 0xDA) || (c >= 0xDF && c <= 0xFB);
        }
    }
    const bool cp874 = thai * 2 > high;
    std::string out;
    for (char ch : s) {
        const auto c = static_cast<unsigned char>(ch);
        if (c < 0x80)
            out += ch;
        else if (cp874 && c >= 0xA1 && c <= 0xDA)
            appendUtf8(out, 0x0E01 + (c - 0xA1));
        else if (cp874 && c >= 0xDF && c <= 0xFB)
            appendUtf8(out, 0x0E3F + (c - 0xDF));
        else if (cp874 && c == 0x80)
            appendUtf8(out, 0x20AC);
        else if (cp874 && c == 0xA0)
            appendUtf8(out, 0x00A0);
        else if (!cp874 && c >= 0xA0)
            appendUtf8(out, c);
        else
            appendUtf8(out, 0xFFFD);
    }
    return out;
}

}  // namespace

static std::vector<std::vector<std::string>> parseCsvUtf8(std::string_view csv);

std::vector<std::vector<std::string>> parseCsv(std::string_view csv) {
    if (csv.size() >= 2 && static_cast<unsigned char>(csv[0]) == 0xFF &&
        static_cast<unsigned char>(csv[1]) == 0xFE)
        return parseCsvUtf8(fromUtf16(csv, false));
    if (csv.size() >= 2 && static_cast<unsigned char>(csv[0]) == 0xFE &&
        static_cast<unsigned char>(csv[1]) == 0xFF)
        return parseCsvUtf8(fromUtf16(csv, true));
    if (!validUtf8(csv))
        return parseCsvUtf8(fromLegacy(csv));
    return parseCsvUtf8(csv);
}

std::string toCsv(const TableData& t) {
    std::string out;
    const std::size_t rows = rowCount(t), cols = colCount(t);
    for (std::size_t r = 0; r < rows; ++r) {
        for (std::size_t c = 0; c < cols; ++c) {
            const TableCell& cell = cellAt(t, r, c);
            if (c)
                out += ',';
            if (!cell.covered)
                out += quoteField(text::plainText(displayText(t, r, c)));
        }
        out += "\r\n";
    }
    return out;
}

static std::vector<std::vector<std::string>> parseCsvUtf8(std::string_view csv) {
    std::vector<std::vector<std::string>> rows;
    if (csv.size() >= 3 && csv.substr(0, 3) == "\xEF\xBB\xBF")
        csv.remove_prefix(3);
    // The separator is the comma, or the semicolon when the first line has more of those.
    char sep = ',';
    {
        std::size_t commas = 0, semis = 0, tabs = 0;
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
            else if (!q && c == '\t')
                ++tabs;
        }
        if (tabs > 0 && tabs >= commas && tabs >= semis)  // cells copied from Excel or a web table
            sep = '\t';
        else if (semis > commas)
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
