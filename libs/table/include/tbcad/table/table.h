#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "tbcad/core/core.h"
#include "tbcad/geom/entity.h"

// Tables (Q71). A TableData is a grid in its own frame: x to the right, y downwards from the
// top-left corner `origin`, the whole frame rotated by `rotation`. Behaviour is in docs/table.md.
namespace tbcad::table {

struct CellRef {
    std::size_t row = 0, col = 0;
    bool operator==(const CellRef&) const = default;
};

// Rectangle in the table frame: x0 < x1, y0 < y1 (y downwards).
struct Rect {
    double x0 = 0, y0 = 0, x1 = 0, y1 = 0;
};

inline std::size_t rowCount(const TableData& t) {
    return t.rowHeights.size();
}
inline std::size_t colCount(const TableData& t) {
    return t.colWidths.size();
}
TableCell& cellAt(TableData& t, std::size_t row, std::size_t col);
const TableCell& cellAt(const TableData& t, std::size_t row, std::size_t col);

double totalWidth(const TableData& t);
double totalHeight(const TableData& t);

// Table frame <-> world.
Vec2 toWorld(const TableData& t, Vec2 local);
Vec2 toLocal(const TableData& t, Vec2 world);

// Area of a cell including its merge. For a covered cell, the area of the cell that covers it.
Rect cellRect(const TableData& t, std::size_t row, std::size_t col);

// A new table. `title` non-empty adds a merged first row holding it. Sizes are clamped to sane
// limits (1..1000 columns, 1..100000 rows); widths and heights must be positive.
TableData make(std::size_t rows, std::size_t cols, double colWidth, double rowHeight,
               const std::string& title, Vec2 origin, double textHeight);

// The anchor cell under a world point, if the point is inside the table.
std::optional<CellRef> hit(const TableData& t, Vec2 world);
// The anchor cell of the merge that covers (row, col); the cell itself when it is not covered.
CellRef anchorOf(const TableData& t, std::size_t row, std::size_t col);

// ---- editing: each returns false (and leaves the table untouched) when it cannot ------------

bool setText(TableData& t, std::size_t row, std::size_t col, const std::string& utf8);
// Inserting or deleting first unmerges every merged range that crosses the affected line.
bool insertRow(TableData& t, std::size_t at, double height = 0);
bool deleteRow(TableData& t, std::size_t at);
bool insertCol(TableData& t, std::size_t at, double width = 0);
bool deleteCol(TableData& t, std::size_t at);
// Merges the rectangle from (r0,c0) to (r1,c1) (any corner order) into its top-left cell, whose
// text is kept; the texts of the other cells are joined into it with a space when not empty.
bool merge(TableData& t, CellRef a, CellRef b);
bool unmerge(TableData& t, std::size_t row, std::size_t col);
bool setColumnWidth(TableData& t, std::size_t col, double width);
bool setRowHeight(TableData& t, std::size_t row, double height);

// ---- table styles (TABLESTYLE) ---------------------------------------------------------------

// A named set of the sizes a new table starts with. Kept in Header::custom, so it is saved with the
// drawing. "Standard" always exists (30 x 8, text 2.8, margin 1) and can be redefined, not deleted.
struct TableStyle {
    std::string name{};
    double colWidth = 30;
    double rowHeight = 8;
    double textHeight = 2.8;
    double margin = 1.0;
    std::string textStyle{};  // text style name; empty = the drawing's current one
};
// The name without characters a style name cannot hold, trimmed; empty when nothing is left.
std::string validStyleName(const std::string& raw);
std::vector<TableStyle> tableStyles(const Header& h);  // Standard first
std::optional<TableStyle> findTableStyle(const Header& h, const std::string& name);  // any case
void setTableStyle(Header& h, const TableStyle& s);  // adds or replaces
bool deleteTableStyle(Header& h, const std::string& name);
std::string currentTableStyleName(const Header& h);
void setCurrentTableStyle(Header& h, const std::string& name);

// ---- formulas -------------------------------------------------------------------------------

// A cell whose text starts with "=" is a formula, worked out when the table is drawn, exported
// to CSV or exploded: + - * / ^, brackets, numbers, cell names (A1: column letters, row number
// from 1) and the functions SUM, AVERAGE (AVG), MIN, MAX, COUNT over cells, ranges (A1:C3) and
// numbers, plus ABS, SQRT and ROUND(x, digits). A cell holding text that reads as a number counts
// as that number; an empty cell as 0. Mistakes show as #ERR, #REF (outside the table), #NAME,
// #VALUE (text where a number is needed), #DIV/0 or #LOOP (a formula that needs itself).
bool isFormula(const std::string& text);
std::string cellName(std::size_t row, std::size_t col);  // 0,0 -> "A1"
// What the cell shows: its text, or the value of its formula.
std::string displayText(const TableData& t, std::size_t row, std::size_t col);

// ---- text form (kept in DXF extended data so a re-import gets the table back) -----------------

// ASCII-only, one line. The text style is not part of it.
std::string serialize(const TableData& t);
std::optional<TableData> parse(std::string_view s);

// ---- CSV (TABLEEXPORT, TABLEIMPORT) ---------------------------------------------------------

// The cells as plain text, one CSV row per table row, CRLF line ends; covered (merged-over) cells
// are empty fields.
std::string toCsv(const TableData& t);
// Rows of fields; understands quotes, doubled quotes, CRLF or LF, a UTF-8 byte order mark and the
// semicolon as well as the comma (Excel in some locales). Blank lines are skipped.
std::vector<std::vector<std::string>> parseCsv(std::string_view csv);
// A table holding the rows, each column as wide as its longest text.
TableData fromRows(const std::vector<std::vector<std::string>>& rows, Vec2 origin,
                   double textHeight, const TextStyle* style);

// ---- geometry -------------------------------------------------------------------------------

struct TextItem {
    Vec2 position;  // world, the anchor point of `attach`
    double width = 0;
    double rotation = 0;
    Attachment attach = Attachment::MiddleLeft;
    std::string utf8;
};
struct Layout {
    std::vector<std::pair<Vec2, Vec2>> lines;  // world, grid and border segments
    std::vector<TextItem> texts;
};
Layout layout(const TableData& t);

BBox2 bounds(const TableData& t);

void registerDisplay();
void buildTableDisplay(const Document& doc, const Entity& e, const geom::DisplayContext& ctx,
                       DisplayList& out);
// Lines for the grid and one MText per non-empty cell, with the table's own properties.
Result<std::vector<Entity>> explodeTable(const Document& doc, const Entity& e);

}  // namespace tbcad::table
