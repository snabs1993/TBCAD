#include <algorithm>
#include <cmath>

#include "tbcad/blocks/tbcad_blocks.h"
#include "tbcad/geom/geom.h"
#include "tbcad/table/table.h"
#include "tbcad/tablecmd/tablecmd.h"

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
        dop.defaultValue = 30.0;
        auto cw = co_await ctx.getDistance("Column width <30>", dop);
        if (!cw.ok())
            co_return;
        dop.defaultValue = 8.0;
        auto rh = co_await ctx.getDistance("Row height <8>", dop);
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
        const double textHeight = rh.value * 0.35;
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
        t.style = ctx.doc().header().currentTextStyle;
        Entity e;
        e.data = Indirect<TableData>(std::move(t));
        auto id = ctx.tx().add(std::move(e));
        if (!id)
            ctx.message(id.error().message);
    }
};

// ---- TABLEEDIT ---------------------------------------------------------------------------------

class TableEditCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "TABLEEDIT"; }
    std::vector<std::string> defaultAliases() const override { return {"TE"}; }
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
            po.keywords = {"Text",      "RowBelow", "RowAbove", "ColRight", "ColLeft", "DeleteRow",
                           "DeleteCol", "Merge",    "Unmerge",  "Width",    "Height"};
            auto pick = co_await ctx.getPoint(
                "Click a cell, or [Text/RowBelow/RowAbove/ColRight/ColLeft/DeleteRow/DeleteCol/"
                "Merge/Unmerge/Width/Height] <done>",
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
            const std::string& k = pick.keyword;
            if (k == "Text") {
                cmd::StringOptions so;
                so.allowEmpty = true;
                so.defaultValue = table::cellAt(*t, cur.row, cur.col).utf8;
                auto s = co_await ctx.getString("Cell text (row " + std::to_string(cur.row + 1) +
                                                    ", column " + std::to_string(cur.col + 1) +
                                                    "), - clears",
                                                so);
                if (s.ok()) {
                    const std::string text = s.text == "-" ? std::string() : s.text;
                    edit([&](TableData& d) { table::setText(d, cur.row, cur.col, text); });
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

}  // namespace

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
}

}  // namespace tbcad::tablecmd
