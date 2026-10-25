#include <algorithm>
#include <cmath>

#include "tbcad/blocks/tbcad_blocks.h"
#include "tbcad/geom/geom.h"
#include "tbcad/table/table.h"
#include "tbcad/tablecmd/tablecmd.h"
#include "tbcad/ui/ui_registry.h"

namespace tbcad::tablecmd {

namespace {

void previewTable(cmd::CommandContext& ctx, const TableData& t, DisplayList& out) {
    Entity e;
    e.common.layer = ctx.doc().header().currentLayer;
    e.data = Indirect<TableData>(t);
    geom::DisplayContext dc;
    const double scale = ctx.host().view().scale;
    if (scale > 0)
        dc.pixelSizeMm = scale;
    try {
        geom::buildDisplay(ctx.doc(), e, dc, out);
    } catch (...) {
        // a half-defined preview simply shows nothing
    }
}

std::string shortNumber(double v) {
    char buf[48];
    std::snprintf(buf, sizeof buf, "%g", v);
    return buf;
}

bool isTable(const Entity& e) {
    return kind(e) == EntityKind::Table;
}

// ---- TABLE -------------------------------------------------------------------------------------

class TableCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "TABLE"; }
    std::vector<std::string> defaultAliases() const override { return {"TB"}; }
    std::string_view description() const override { return "Insert an empty table"; }

    cmd::Task run(cmd::CommandContext& ctx) override {
        using cmd::Status;
        // Sizes come from the current table style.
        const table::TableStyle style =
            table::findTableStyle(ctx.doc().header(),
                                  table::currentTableStyleName(ctx.doc().header()))
                .value_or(table::TableStyle{});
        cmd::IntOptions io;
        io.min = 1;
        io.max = 100;
        io.defaultValue = 4;
        auto cols = co_await ctx.getInteger("Number of columns <4>", io);
        if (!cols.ok())
            co_return;
        io.max = 1000;
        io.defaultValue = 5;
        auto rows = co_await ctx.getInteger("Number of rows <5>", io);
        if (!rows.ok())
            co_return;
        cmd::DistanceOptions dop;
        dop.allowZero = false;
        dop.defaultValue = style.colWidth;
        auto cw =
            co_await ctx.getDistance("Column width <" + shortNumber(style.colWidth) + ">", dop);
        if (!cw.ok())
            co_return;
        dop.defaultValue = style.rowHeight;
        auto rh =
            co_await ctx.getDistance("Row height <" + shortNumber(style.rowHeight) + ">", dop);
        if (!rh.ok())
            co_return;
        cmd::StringOptions so;
        so.allowEmpty = true;
        auto title = co_await ctx.getString("Title <none>", so);
        if (title.status == Status::Cancel)
            co_return;
        const std::string titleText = title.ok() ? title.text : std::string();

        const std::size_t nrows =
            static_cast<std::size_t>(rows.value) + (titleText.empty() ? 0u : 1u);
        const double textHeight = rh.value == style.rowHeight
                                      ? style.textHeight
                                      : style.textHeight * rh.value / style.rowHeight;
        auto build = [=](Vec2 at) {
            return table::make(nrows, static_cast<std::size_t>(cols.value), cw.value, rh.value,
                               titleText, at, textHeight);
        };
        cmd::PointOptions po;
        auto* c = &ctx;
        po.preview = [c, build](Vec2 cursor, DisplayList& out) {
            previewTable(*c, build(cursor), out);
        };
        auto where = co_await ctx.getPoint("Specify insertion point (top-left corner)", po);
        if (!where.ok())
            co_return;
        ctx.setUndoLabel("TABLE");
        TableData t = build(where.point);
        t.margin = style.margin;
        t.headerRow = style.headerRow;
        t.titleStyle = style.title;
        t.headerStyle = style.header;
        t.dataStyle = style.data;
        t.style = ctx.doc().header().currentTextStyle;
        if (!style.textStyle.empty())
            if (const TextStyle* ts = ctx.doc().textStyleByName(style.textStyle))
                t.style = ts->id;
        Entity e;
        e.data = Indirect<TableData>(std::move(t));
        applyAnnotativeDefault(ctx.doc().header(), e);
        auto id = ctx.tx().add(std::move(e));
        if (!id)
            ctx.message(id.error().message);
    }
};

// ---- TABLEEDIT ---------------------------------------------------------------------------------

class TableEditCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "TABLEEDIT"; }
    std::vector<std::string> defaultAliases() const override { return {"TE", "TABLEDIT"}; }
    std::string_view description() const override {
        return "Edit cell text, merge cells, add or remove rows and columns";
    }

    cmd::Task run(cmd::CommandContext& ctx) override {
        using cmd::Status;
        cmd::EntityOptions eo;
        eo.filter = isTable;
        auto o = co_await ctx.getEntity("Select table", eo);
        if (!o.ok())
            co_return;
        const EntityId id = o.id;
        ctx.setUndoLabel("TABLEEDIT");

        table::CellRef cur{0, 0};
        if (const Entity* e = ctx.doc().find(id))
            if (const auto* t = entityAs<TableData>(*e))
                if (auto h = table::hit(*t, o.pickPoint))
                    cur = *h;

        auto edit = [&](auto&& fn) {
            auto r = ctx.tx().modify(id, [&](Entity& ent) {
                if (auto* t = entityAs<TableData>(ent))
                    fn(*t);
            });
            if (!r)
                ctx.message(r.error().message);
        };

        for (;;) {
            const Entity* e = ctx.doc().find(id);
            const auto* t = e ? entityAs<TableData>(*e) : nullptr;
            if (!t)
                co_return;
            cur.row = std::min(cur.row, table::rowCount(*t) - 1);
            cur.col = std::min(cur.col, table::colCount(*t) - 1);

            cmd::PointOptions po;
            po.keywords = {"Text",      "RowBelow",  "RowAbove",   "ColRight", "ColLeft",
                           "DeleteRow", "DeleteCol", "Merge",      "Unmerge",  "Width",
                           "Height",    "Align",     "AlignColumn", "Next",      "Previous"};
            auto pick = co_await ctx.getPoint(
                "Click a cell, or [Text/RowBelow/RowAbove/ColRight/ColLeft/DeleteRow/DeleteCol/"
                "Merge/Unmerge/Width/Height/Align/AlignColumn/Next/Previous] <done>",
                po);
            if (pick.status == Status::None || pick.status == Status::Cancel)
                co_return;
            if (pick.ok()) {
                if (auto h = table::hit(*t, pick.point)) {
                    cur = *h;
                    // Clicking a cell goes straight to its text.
                    pick.status = Status::Keyword;
                    pick.keyword = "Text";
                } else {
                    ctx.message("That point is outside the table.");
                    continue;
                }
            }
            std::string k = pick.keyword;
            if (k == "Next" || k == "Previous") {
                // Tab / Shift+Tab: step to the neighbouring cell (wrapping across rows), then edit it.
                const std::size_t cols = table::colCount(*t), rows = table::rowCount(*t);
                std::size_t idx = cur.row * cols + cur.col;
                idx = k == "Next" ? (idx + 1) % (rows * cols) : (idx + rows * cols - 1) % (rows * cols);
                cur.row = idx / cols;
                cur.col = idx % cols;
                k = "Text";
            }
            if (k == "Text") {
                cmd::StringOptions so;
                so.allowEmpty = true;
                so.defaultValue = table::cellBody(table::cellAt(*t, cur.row, cur.col).utf8);
                const auto keepAlign = table::cellAlign(table::cellAt(*t, cur.row, cur.col));
                auto s = co_await ctx.getString("Cell text (row " + std::to_string(cur.row + 1) +
                                                    ", column " + std::to_string(cur.col + 1) +
                                                    "), - clears",
                                                so);
                if (s.ok()) {
                    const std::string text = s.text == "-" ? std::string() : s.text;
                    edit([&](TableData& d) {
                        table::setText(d, cur.row, cur.col, text);
                        if (keepAlign)
                            table::setCellAlign(d, cur.row, cur.col, keepAlign);
                    });
                }
            } else if (k == "RowBelow" || k == "RowAbove") {
                const std::size_t at = k == "RowBelow" ? cur.row + 1 : cur.row;
                edit([&](TableData& d) { table::insertRow(d, at); });
            } else if (k == "ColRight" || k == "ColLeft") {
                const std::size_t at = k == "ColRight" ? cur.col + 1 : cur.col;
                edit([&](TableData& d) { table::insertCol(d, at); });
            } else if (k == "DeleteRow") {
                edit([&](TableData& d) {
                    if (!table::deleteRow(d, cur.row))
                        ctx.message("The last row cannot be deleted.");
                });
            } else if (k == "DeleteCol") {
                edit([&](TableData& d) {
                    if (!table::deleteCol(d, cur.col))
                        ctx.message("The last column cannot be deleted.");
                });
            } else if (k == "Merge") {
                auto other = co_await ctx.getPoint("Click the opposite corner cell to merge to");
                if (other.ok()) {
                    const Entity* e2 = ctx.doc().find(id);
                    const auto* t2 = e2 ? entityAs<TableData>(*e2) : nullptr;
                    auto h = t2 ? table::hit(*t2, other.point) : std::nullopt;
                    if (!h) {
                        ctx.message("That point is outside the table.");
                    } else {
                        const table::CellRef from = cur, to = *h;
                        edit([&](TableData& d) {
                            if (!table::merge(d, from, to))
                                ctx.message("Those cells cannot be merged.");
                        });
                    }
                }
            } else if (k == "Unmerge") {
                edit([&](TableData& d) {
                    if (!table::unmerge(d, cur.row, cur.col))
                        ctx.message("That cell is not merged.");
                });
            } else if (k == "Align" || k == "AlignColumn") {
                cmd::KeywordOptions ko;
                ko.keywords = {"Left", "Center", "Right", "Default"};
                ko.defaultKeyword = "Left";
                auto a =
                    co_await ctx.getKeyword("Alignment [Left/Center/Right/Default] <Left>", ko);
                if (a.ok()) {
                    std::optional<table::CellAlign> al;
                    if (a.keyword == "Left")
                        al = table::CellAlign::Left;
                    else if (a.keyword == "Center")
                        al = table::CellAlign::Center;
                    else if (a.keyword == "Right")
                        al = table::CellAlign::Right;
                    const bool column = k == "AlignColumn";
                    edit([&](TableData& d) {
                        if (!column) {
                            table::setCellAlign(d, cur.row, cur.col, al);
                            return;
                        }
                        for (std::size_t r = 0; r < table::rowCount(d); ++r)
                            table::setCellAlign(d, r, cur.col, al);
                    });
                }
            } else if (k == "Width" || k == "Height") {
                cmd::DistanceOptions dop;
                dop.allowZero = false;
                auto v = co_await ctx.getDistance(
                    k == "Width" ? "New column width" : "New row height", dop);
                if (v.ok()) {
                    const double val = v.value;
                    edit([&](TableData& d) {
                        if (k == "Width")
                            table::setColumnWidth(d, cur.col, val);
                        else
                            table::setRowHeight(d, cur.row, val);
                    });
                }
            }
        }
    }
};

// ---- TABLESTYLE --------------------------------------------------------------------------------

// TABLESTYLE: named sets of sizes new tables start with. Set makes one current; New and Modify
// ask for the sizes; Apply gives the sizes of a style to tables already drawn.
class TableStyleCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "-TABLESTYLE"; }
    std::string_view description() const override {
        return "Define table styles and choose the one new tables use";
    }

    cmd::Task run(cmd::CommandContext& ctx) override {
        using cmd::Status;
        const std::string cur = table::currentTableStyleName(ctx.doc().header());
        cmd::KeywordOptions ko;
        ko.keywords = {"Set", "New", "Modify", "Delete", "Apply", "List"};
        ko.defaultKeyword = "List";
        auto k = co_await ctx.getKeyword(
            "Table style [Set/New/Modify/Delete/Apply/List] <List>, current is " + cur, ko);
        if (!k.ok())
            co_return;
        if (k.keyword == "List") {
            for (const table::TableStyle& s : table::tableStyles(ctx.doc().header()))
                ctx.message((s.name == cur ? "* " : "  ") + s.name + ": columns " +
                            shortNumber(s.colWidth) + " x rows " + shortNumber(s.rowHeight) +
                            ", text " + shortNumber(s.textHeight) + ", margin " +
                            shortNumber(s.margin) +
                            (s.textStyle.empty() ? "" : ", text style " + s.textStyle) +
                            (s.headerRow ? ", header row" : "") +
                            (s.title.isDefault() && s.header.isDefault() && s.data.isDefault()
                                 ? ""
                                 : ", cell styles"));
            co_return;
        }
        auto n = co_await ctx.getString("Style name");
        if (!n.ok())
            co_return;
        const std::string name = table::validStyleName(n.text);
        if (name.empty()) {
            ctx.message("A style needs a name.");
            co_return;
        }
        auto existing = table::findTableStyle(ctx.doc().header(), name);
        if (k.keyword == "Set") {
            if (!existing) {
                ctx.message("No table style named \"" + name + "\".");
                co_return;
            }
            ctx.setUndoLabel("TABLESTYLE");
            const std::string set = existing->name;
            (void)ctx.tx().modifyHeader([&](Header& h) { table::setCurrentTableStyle(h, set); });
            ctx.message("Table style " + set + " is current.");
        } else if (k.keyword == "Delete") {
            if (!existing || existing->name == "Standard") {
                ctx.message(existing ? "Standard cannot be deleted."
                                     : "No table style named \"" + name + "\".");
                co_return;
            }
            ctx.setUndoLabel("TABLESTYLE");
            const std::string del = existing->name;
            (void)ctx.tx().modifyHeader([&](Header& h) { table::deleteTableStyle(h, del); });
        } else if (k.keyword == "New" || k.keyword == "Modify") {
            if (k.keyword == "New" && existing) {
                ctx.message("A table style named \"" + existing->name + "\" exists; use Modify.");
                co_return;
            }
            if (k.keyword == "Modify" && !existing) {
                ctx.message("No table style named \"" + name + "\".");
                co_return;
            }
            table::TableStyle s = existing.value_or(table::TableStyle{});
            s.name = existing ? existing->name : name;
            cmd::DistanceOptions dop;
            dop.allowZero = false;
            dop.defaultValue = s.colWidth;
            auto cw =
                co_await ctx.getDistance("Column width <" + shortNumber(s.colWidth) + ">", dop);
            if (!cw.ok())
                co_return;
            dop.defaultValue = s.rowHeight;
            auto rh =
                co_await ctx.getDistance("Row height <" + shortNumber(s.rowHeight) + ">", dop);
            if (!rh.ok())
                co_return;
            dop.defaultValue = s.textHeight;
            auto th =
                co_await ctx.getDistance("Text height <" + shortNumber(s.textHeight) + ">", dop);
            if (!th.ok())
                co_return;
            dop.defaultValue = s.margin;
            dop.allowZero = true;
            auto mg = co_await ctx.getDistance("Cell margin <" + shortNumber(s.margin) + ">", dop);
            if (!mg.ok())
                co_return;
            cmd::StringOptions so;
            so.allowEmpty = true;
            so.defaultValue = s.textStyle;
            auto ts = co_await ctx.getString(
                "Text style name <" + (s.textStyle.empty() ? std::string("current") : s.textStyle) +
                    ">, - for the current one",
                so);
            if (!ts.ok() && ts.status != Status::None)
                co_return;
            s.colWidth = cw.value;
            s.rowHeight = rh.value;
            s.textHeight = th.value;
            s.margin = mg.value;
            // Cell styles: one role at a time, until Done.
            for (;;) {
                cmd::KeywordOptions cko;
                cko.keywords = {"Title", "Header", "Data", "HeaderRow", "Done"};
                cko.defaultKeyword = "Done";
                auto role = co_await ctx.getKeyword(
                    "Cell styles [Title/Header/Data/HeaderRow/Done] <Done>", cko);
                if (!role.ok() || role.keyword == "Done")
                    break;
                if (role.keyword == "HeaderRow") {
                    cmd::KeywordOptions yn;
                    yn.keywords = {"Yes", "No"};
                    yn.defaultKeyword = s.headerRow ? "Yes" : "No";
                    auto h = co_await ctx.getKeyword("Table has a header row", yn);
                    if (h.ok())
                        s.headerRow = h.keyword == "Yes";
                    continue;
                }
                TableCellStyle& cs = role.keyword == "Title"    ? s.title
                                     : role.keyword == "Header" ? s.header
                                                                : s.data;
                cmd::RealOptions ro;
                ro.defaultValue = cs.textHeight;
                ro.min = 0;
                auto ch = co_await ctx.getReal(
                    "Text height, 0 for the table's <" + shortNumber(cs.textHeight) + ">", ro);
                if (!ch.ok())
                    continue;
                cmd::IntOptions io;
                io.min = 0;
                io.max = 255;
                io.defaultValue = cs.color;
                auto cc = co_await ctx.getInteger(
                    "Text colour index 1-255, 0 as the text <" + std::to_string(cs.color) + ">",
                    io);
                if (!cc.ok())
                    continue;
                io.defaultValue = cs.fill;
                auto cf = co_await ctx.getInteger(
                    "Fill colour index 1-255, 0 none <" + std::to_string(cs.fill) + ">", io);
                if (!cf.ok())
                    continue;
                cmd::KeywordOptions ako;
                ako.keywords = {"Default", "Left", "Center", "Right"};
                ako.defaultKeyword = cs.align == 0   ? "Left"
                                     : cs.align == 1 ? "Center"
                                     : cs.align == 2 ? "Right"
                                                     : "Default";
                auto ca = co_await ctx.getKeyword("Alignment", ako);
                if (!ca.ok())
                    continue;
                cs.textHeight = std::max(0.0, ch.value);
                cs.color = static_cast<int>(cc.value);
                cs.fill = static_cast<int>(cf.value);
                cs.align = ca.keyword == "Left"     ? 0
                           : ca.keyword == "Center" ? 1
                           : ca.keyword == "Right"  ? 2
                                                    : -1;
            }
            if (ts.ok()) {
                if (ts.text == "-") {
                    s.textStyle.clear();
                } else if (const TextStyle* found = ctx.doc().textStyleByName(ts.text)) {
                    s.textStyle = found->name;
                } else if (!ts.text.empty()) {
                    ctx.message("No text style named \"" + ts.text +
                                "\"; the style keeps the current one.");
                    s.textStyle.clear();
                }
            }
            ctx.setUndoLabel("TABLESTYLE");
            (void)ctx.tx().modifyHeader([&](Header& h) { table::setTableStyle(h, s); });
            ctx.message("Table style " + s.name + (existing ? " changed." : " created."));
        } else {  // Apply
            if (!existing) {
                ctx.message("No table style named \"" + name + "\".");
                co_return;
            }
            auto sel = co_await ctx.getSelection("Select tables to restyle",
                                                 cmd::SelectionOptions{.filter = isTable});
            if (!sel.ok())
                co_return;
            const table::TableStyle s = *existing;
            const TextStyle* ts =
                s.textStyle.empty() ? nullptr : ctx.doc().textStyleByName(s.textStyle);
            ctx.setUndoLabel("TABLESTYLE");
            int n2 = 0;
            for (EntityId id : sel.ids) {
                auto r = ctx.tx().modify(id, [&](Entity& e) {
                    if (auto* t = entityAs<TableData>(e)) {
                        for (double& w : t->colWidths)
                            w = s.colWidth;
                        for (double& h : t->rowHeights)
                            h = s.rowHeight;
                        t->textHeight = s.textHeight;
                        t->margin = s.margin;
                        t->headerRow = s.headerRow;
                        t->titleStyle = s.title;
                        t->headerStyle = s.header;
                        t->dataStyle = s.data;
                        if (ts)
                            t->style = ts->id;
                    }
                });
                if (r)
                    ++n2;
            }
            ctx.message("Style " + s.name + " applied to " + std::to_string(n2) + " tables.");
        }
    }
};

// TABLESTYLE [TS]: the Table Style dialog; it edits through -TABLESTYLE.
class TableStylePanelCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "TABLESTYLE"; }
    std::vector<std::string> defaultAliases() const override { return {"TS"}; }
    std::string_view description() const override { return "Open the Table Style dialog"; }
    cmd::Task run(cmd::CommandContext& ctx) override {
        ctx.host().openPanel("tablestyles");
        co_return;
    }
};

}  // namespace

#ifdef TBCAD_TABLECMD_WITH_IMGUI
void registerTableStylePanel(ui::UiRegistry& ui);  // ui/style_panel.cpp
#endif

void registerCommands(cmd::Registry& registry) {
    table::registerDisplay();
    blocks::ExplodeRegistry::set(
        EntityKind::Table,
        [](const Document& doc, const Entity& e) -> Result<blocks::ExplodeResult> {
            auto r = table::explodeTable(doc, e);
            if (!r)
                return r.error();
            return blocks::ExplodeResult{std::move(r).value(), {}};
        });
    (void)registry.add(std::make_unique<TableCommand>());
    (void)registry.add(std::make_unique<TableEditCommand>());
    (void)registry.add(std::make_unique<TableStyleCommand>());
    (void)registry.add(std::make_unique<TableStylePanelCommand>());
    registerCsvCommands(registry);
    registerDataLinkCommands(registry);
}

void registerUi(ui::UiRegistry& ui) {
    ui.addMenuItem("Draw/Table", "TABLE");
    ui.addMenuItem("Modify/Edit Table", "TABLEEDIT");
    ui.addMenuItem("Format/Table Style...", "TABLESTYLE");
    ui.addMenuItem("Draw/Table from CSV...", "TABLEIMPORT");
    ui.addMenuItem("Modify/Export Table to CSV...", "TABLEEXPORT");
#ifdef TBCAD_TABLECMD_WITH_IMGUI
    registerTableStylePanel(ui);
#endif
}

}  // namespace tbcad::tablecmd
