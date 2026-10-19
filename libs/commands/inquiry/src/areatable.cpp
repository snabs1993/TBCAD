#include <algorithm>
#include <cmath>

#include "extra.h"
#include "tbcad/geom/geom.h"

namespace tbcad::inquiry {

namespace {

const char* const kKindNames[] = {"Line",    "Circle",    "Arc",    "Polyline", "Point",
                                  "Ellipse", "Spline",    "Text",   "MText",    "Hatch",
                                  "Block",   "Dimension", "Leader", "Proxy"};

bool isClosedShape(const Entity& e) {
    if (const auto* pl = entityAs<PolylineData>(e))
        return pl->closed && pl->v.size() >= 3;
    return areaOfEntity(e).has_value() && kind(e) != EntityKind::Polyline;
}

std::string numberText(double v, Precision p) {
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.*f", std::clamp(p, 0, 12), v);
    return buf;
}

}  // namespace

std::vector<EntityData> buildAreaTable(const Document& doc,
                                       const std::vector<const Entity*>& shapes, Vec2 topLeft,
                                       const AreaTableOptions& options) {
    struct Row {
        std::vector<std::string> cells;
    };
    const std::string unit(unitSuffix(doc.header().units));
    const double k = mmPerUnit(doc.header().units);
    std::vector<Row> rows;
    rows.push_back({{"No.", "Object", "Area (" + unit + "^2)", "Perimeter (" + unit + ")"}});
    double totalA = 0, totalP = 0;
    int n = 0;
    for (const Entity* e : shapes) {
        if (!isClosedShape(*e))
            continue;
        const auto res = areaOfEntity(*e);
        if (!res)
            continue;
        ++n;
        totalA += res->area;
        totalP += res->perimeter;
        rows.push_back({{std::to_string(n), kKindNames[static_cast<std::size_t>(kind(*e))],
                         numberText(res->area / (k * k), options.precision),
                         numberText(res->perimeter / k, options.precision)}});
    }
    std::vector<EntityData> out;
    if (n == 0)
        return out;
    rows.push_back({{"", "Total", numberText(totalA / (k * k), options.precision),
                     numberText(totalP / k, options.precision)}});

    const double th = options.textHeight;
    const double rowH = th * 2;
    const double charW = th * 0.8;
    std::vector<double> colW(4, 0);
    for (const Row& r : rows)
        for (std::size_t c = 0; c < 4; ++c)
            colW[c] = std::max(colW[c], (static_cast<double>(r.cells[c].size()) + 2) * charW);
    double width = 0;
    for (double w : colW)
        width += w;
    const double height = rowH * static_cast<double>(rows.size());

    for (std::size_t i = 0; i <= rows.size(); ++i) {
        const double y = topLeft.y - rowH * static_cast<double>(i);
        out.emplace_back(LineData{{topLeft.x, y}, {topLeft.x + width, y}});
    }
    double x = topLeft.x;
    for (std::size_t c = 0; c <= 4; ++c) {
        out.emplace_back(LineData{{x, topLeft.y}, {x, topLeft.y - height}});
        if (c < 4)
            x += colW[c];
    }
    x = topLeft.x;
    for (std::size_t c = 0; c < 4; ++c) {
        for (std::size_t r = 0; r < rows.size(); ++r) {
            if (rows[r].cells[c].empty())
                continue;
            TextData t;
            t.position =
                t.alignPoint = {x + charW, topLeft.y - rowH * (static_cast<double>(r) + 0.5)};
            t.height = th;
            t.v = VAlign::Middle;
            t.utf8 = rows[r].cells[c];
            out.emplace_back(std::move(t));
        }
        x += colW[c];
    }
    return out;
}

namespace {

// AREATABLE [AT]: a table of the area and perimeter of the selected closed shapes, drawn as lines
// and text at a picked corner.
class AreaTable final : public cmd::Command {
public:
    std::string_view name() const override { return "AREATABLE"; }
    std::vector<std::string> defaultAliases() const override { return {"AT"}; }
    std::string_view description() const override {
        return "Draw a table of the area and perimeter of closed shapes";
    }

    cmd::Task run(cmd::CommandContext& ctx) override {
        auto sel = co_await ctx.getSelection("Select closed shapes");
        if (!sel.ok() || sel.ids.empty())
            co_return;
        std::vector<const Entity*> shapes;
        for (EntityId id : sel.ids)
            if (const Entity* e = ctx.doc().find(id))
                if (isClosedShape(*e))
                    shapes.push_back(e);
        if (shapes.empty()) {
            ctx.message("None of the selected objects is a closed shape.");
            co_return;
        }
        AreaTableOptions opt;
        if (auto h = ctx.host().setting("areatable.textheight")) {
            try {
                opt.textHeight = std::max(0.01, std::stod(*h));
            } catch (...) {
            }
        }
        opt.precision = linearPrecision(ctx.doc().header());
        if (auto s = ctx.host().setting("inquiry.precision")) {
            try {
                opt.precision = std::clamp(std::stoi(*s), 0, 12);
            } catch (...) {
            }
        }
        cmd::PointOptions po;
        po.allowNone = false;
        auto* c = &ctx;
        po.preview = [c, shapes, opt](Vec2 cursor, DisplayList& out) {
            geom::DisplayContext dc;
            for (EntityData& d : buildAreaTable(c->doc(), shapes, cursor, opt)) {
                if (!std::holds_alternative<LineData>(d))
                    continue;
                Entity e{{}, std::move(d)};
                e.common.layer = c->doc().header().currentLayer;
                try {
                    geom::buildDisplay(c->doc(), e, dc, out);
                } catch (...) {
                }
            }
        };
        auto at = co_await ctx.getPoint("Specify the top-left corner of the table", std::move(po));
        if (!at.ok())
            co_return;
        for (EntityData& d : buildAreaTable(ctx.doc(), shapes, at.point, opt)) {
            Entity created{{}, std::move(d)};
            applyCurrentProperties(ctx.doc().header(), created);
            auto id = ctx.tx().add(std::move(created));
            if (!id) {
                ctx.message(id.error().message);
                co_return;
            }
        }
        ctx.message("Area table created for " + std::to_string(shapes.size()) + " shape(s).");
    }
};

}  // namespace

std::unique_ptr<cmd::Command> makeAreaTable() {
    return std::make_unique<AreaTable>();
}

}  // namespace tbcad::inquiry
