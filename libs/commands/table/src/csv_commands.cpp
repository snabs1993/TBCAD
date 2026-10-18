// TABLEEXPORT and TABLEIMPORT: tables to and from CSV files that Excel opens.
#include <filesystem>
#include <fstream>
#include <sstream>

#include "tbcad/geom/geom.h"
#include "tbcad/table/table.h"
#include "tbcad/tablecmd/tablecmd.h"
#include "tbcad/text/annotation_size.h"
#include "tbcad/text/text.h"

namespace tbcad::tablecmd {

namespace {

std::filesystem::path pathOf(const std::string& s) {
    return std::filesystem::path(std::u8string(s.begin(), s.end()));
}

class TableExportCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "TABLEEXPORT"; }
    std::string_view description() const override { return "Write a table to a CSV file"; }

    cmd::Task run(cmd::CommandContext& ctx) override {
        cmd::EntityOptions eo;
        eo.filter = [](const Entity& e) { return kind(e) == EntityKind::Table; };
        auto o = co_await ctx.getEntity("Select table to export", eo);
        if (!o.ok())
            co_return;
        auto f = co_await ctx.getString("Enter the CSV file name <table.csv>",
                                        cmd::StringOptions{.defaultValue = "table.csv"});
        if (!f.ok())
            co_return;
        const Entity* e = ctx.doc().find(o.id);
        const auto* t = e ? entityAs<TableData>(*e) : nullptr;
        if (!t)
            co_return;
        const std::string file = cmd::trim(f.text);
        std::ofstream out(pathOf(file), std::ios::binary);
        if (!out) {
            ctx.message("Cannot write " + file);
            co_return;
        }
        out << "\xEF\xBB\xBF" << table::toCsv(*t);  // a BOM so Excel reads UTF-8 text correctly
        ctx.message("Wrote " + std::to_string(table::rowCount(*t)) + " rows to " + file);
    }
};

class TableImportCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "TABLEIMPORT"; }
    std::string_view description() const override {
        return "Make a table from a CSV file (Excel: Save As CSV)";
    }

    cmd::Task run(cmd::CommandContext& ctx) override {
        auto f = co_await ctx.getString("Enter the CSV file name");
        if (!f.ok())
            co_return;
        const std::string file = cmd::trim(f.text);
        std::ifstream in(pathOf(file), std::ios::binary);
        if (!in) {
            ctx.message("Cannot read " + file);
            co_return;
        }
        std::ostringstream buf;
        buf << in.rdbuf();
        const auto rows = table::parseCsv(buf.str());
        if (rows.empty()) {
            ctx.message("The file has no rows.");
            co_return;
        }
        constexpr double kBase = 2.5;
        const double textHeight = kBase * text::suggestedAnnotationScale(
                                              ctx.doc(), ctx.host().view().scale,
                                              static_cast<bool>(ctx.host().drawingOwner()), kBase);
        const TextStyle* style = ctx.doc().textStyle(ctx.doc().header().currentTextStyle);
        auto build = [=](Vec2 at) { return table::fromRows(rows, at, textHeight, style); };
        cmd::PointOptions po;
        auto* c = &ctx;
        po.preview = [c, build](Vec2 cursor, DisplayList& out) {
            Entity e;
            e.common.layer = c->doc().header().currentLayer;
            e.data = Indirect<TableData>(build(cursor));
            geom::DisplayContext dc;
            if (const double s = c->host().view().scale; s > 0)
                dc.pixelSizeMm = s;
            try {
                geom::buildDisplay(c->doc(), e, dc, out);
            } catch (...) {
            }
        };
        auto where = co_await ctx.getPoint("Specify insertion point (top-left corner)", po);
        if (!where.ok())
            co_return;
        ctx.setUndoLabel("TABLEIMPORT");
        TableData t = build(where.point);
        t.style = ctx.doc().header().currentTextStyle;
        Entity e;
        e.data = Indirect<TableData>(std::move(t));
        applyCurrentProperties(ctx.doc().header(), e);
        auto id = ctx.tx().add(std::move(e));
        if (!id) {
            ctx.message(id.error().message);
            co_return;
        }
        ctx.host().setSelection({id.value()});
        ctx.message("Table of " + std::to_string(rows.size()) + " rows made from " + file);
    }
};

}  // namespace

void registerCsvCommands(cmd::Registry& registry) {
    (void)registry.add(std::make_unique<TableExportCommand>());
    (void)registry.add(std::make_unique<TableImportCommand>());
}

}  // namespace tbcad::tablecmd
