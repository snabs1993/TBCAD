#include <algorithm>
#include <cmath>

#include "commands.h"
#include "tbcad/blocks/tbcad_blocks.h"
#include "tbcad/geom/entity.h"
#include "tbcad/underlay/pdf.h"

namespace tbcad::block_cmds {

namespace {

// XCLIP [XC]: limits an inserted block or xref to a boundary. The boundary is stored in the
// block's coordinates, so it moves, turns and scales with the insert.
class XClipCommand final : public cmd::Command {
public:
    // `pdf`: PDFCLIP, the same for PDF underlays only.
    explicit XClipCommand(bool pdf = false) : pdf_(pdf) {}
    std::string_view name() const override { return pdf_ ? "PDFCLIP" : "XCLIP"; }
    std::vector<std::string> defaultAliases() const override {
        return pdf_ ? std::vector<std::string>{} : std::vector<std::string>{"XC"};
    }
    std::string_view description() const override {
        return pdf_ ? "Clip a PDF underlay to a rectangle or polygon"
                    : "Clip a block or external reference to a boundary";
    }

    cmd::Task run(cmd::CommandContext& ctx) override {
        Document& doc = ctx.doc();
        cmd::EntityOptions eo;
        eo.allowPreselection = true;  // the reference just placed or picked is still selected
        eo.filter = [&doc, this](const Entity& e) {
            const auto* ins = entityAs<InsertData>(e);
            const BlockDef* def = ins ? doc.block(ins->block) : nullptr;
            return def && !ins->array && (!pdf_ || underlay::isPdfPath(def->xrefPath));
        };
        auto picked = co_await ctx.getEntity(
            pdf_ ? "Select a PDF underlay to clip" : "Select a block or xref to clip", eo);
        if (!picked.ok())
            co_return;
        const EntityId id = picked.id;
        const Entity* e = doc.find(id);
        const auto* ins = e ? entityAs<InsertData>(*e) : nullptr;
        if (!ins)
            co_return;
        const bool has = ins->clip.size() >= 3;
        // A reference with no boundary yet goes straight to drawing one.
        std::string option = "New";
        if (has) {
            auto opt = co_await ctx.getKeyword(
                std::string(pdf_ ? "Clip option [New/ON/OFF/Delete/Invert] <New>"
                                 : "Clip option [New/ON/OFF/Delete/Invert/generate Polyline] "
                                   "<New>"),
                cmd::KeywordOptions{
                    .keywords =
                        pdf_ ? std::vector<std::string>{"New", "ON", "OFF", "Delete", "Invert"}
                             : std::vector<std::string>{"New", "ON", "OFF", "Delete", "Invert",
                                                        "Polyline"},
                    .defaultKeyword = "New"});
            if (!opt.ok())
                co_return;
            option = opt.keyword;
        }
        if (option == "Polyline") {
            // A closed polyline on the current layer along the clip boundary, in world space.
            const BlockDef* bdef = doc.block(ins->block);
            if (!has || !bdef) {
                ctx.message("This object has no clipping boundary");
                co_return;
            }
            const auto xf = blocks::instanceTransform(*ins, bdef->base);
            PolylineData pl;
            pl.closed = true;
            for (const Vec2& q : ins->clip)
                pl.v.push_back(PolyVertex{.p = xf.apply(q)});
            Entity ne{{}, EntityData{std::move(pl)}};
            applyCurrentProperties(doc.header(), ne);
            auto added = ctx.tx().add(std::move(ne));
            if (!added) {
                ctx.message(added.error().message);
                co_return;
            }
            ctx.setUndoLabel("Clip polyline");
            ctx.message("Polyline created along the clipping boundary");
            co_return;
        }
        if (option == "Invert") {
            if (!has) {
                ctx.message("This object has no clipping boundary");
                co_return;
            }
            const bool now = !ins->clipInvert;
            (void)ctx.tx().modify(id, [&](Entity& en) {
                auto* d = entityAs<InsertData>(en);
                d->clipInvert = now;
                d->clipOn = true;
            });
            ctx.setUndoLabel("Invert clip");
            ctx.message(now ? "Clipping inverted: the inside is hidden"
                            : "Clipping restored: the outside is hidden");
            co_return;
        }
        if (option == "ON" || option == "OFF" || option == "Delete") {
            if (!has) {
                ctx.message("This object has no clipping boundary");
                co_return;
            }
            const bool on = option == "ON";
            const bool del = option == "Delete";
            (void)ctx.tx().modify(id, [&](Entity& en) {
                auto* d = entityAs<InsertData>(en);
                if (del) {
                    d->clip.clear();
                    d->clipInvert = false;
                }
                d->clipOn = del ? true : on;
            });
            ctx.setUndoLabel(del ? "Delete clip" : on ? "Clip on" : "Clip off");
            ctx.message(del  ? "Clipping boundary deleted"
                        : on ? "Clipping is on"
                             : "Clipping is off");
            co_return;
        }

        // The boundary: click a corner for a rectangle, or choose a polygon or a drawn object.
        // The frame being drawn follows the cursor.
        std::vector<Vec2> world;
        bool invert = false;
        auto outline = [](const std::vector<Vec2>& pts, bool closed, DisplayList& out) {
            DisplayPolyline pl;
            pl.pts = pts;
            pl.closed = closed;
            out.items.emplace_back(std::move(pl));
        };
        for (;;) {
            cmd::PointOptions fo;
            fo.keywords = {"Polygonal", "Select", "Invert"};
            fo.allowNone = false;
            auto first = co_await ctx.getPoint(
                std::string(invert ? "Hide inside. " : "") +
                    "Specify first corner or [Polygonal/Select boundary/Invert]",
                std::move(fo));
            if (first.status == cmd::Status::Keyword && first.keyword == "Invert") {
                invert = !invert;
                ctx.message(invert ? "The inside of the boundary will be hidden"
                                   : "The outside of the boundary will be hidden");
                continue;
            }
            if (first.status == cmd::Status::Keyword && first.keyword == "Select") {
                // A closed polyline, circle, ellipse or spline already drawn: its outline is the
                // boundary (arcs become short chords).
                cmd::EntityOptions bo;
                bo.filter = [](const Entity& en) {
                    if (const auto* pl = entityAs<PolylineData>(en))
                        return pl->closed && pl->v.size() >= 3;
                    if (const auto* el = entityAs<EllipseData>(en))
                        return std::abs(el->endParam - el->startParam) >= kTwoPi - 1e-9;
                    if (const auto* sp = entityAs<SplineData>(en))
                        return sp->closed || sp->periodic;
                    return entityAs<CircleData>(en) != nullptr;
                };
                auto edge = co_await ctx.getEntity(
                    "Select a closed polyline, circle, ellipse or spline as the boundary", bo);
                if (!edge.ok())
                    co_return;
                const Entity* be = doc.find(edge.id);
                std::vector<Vec2> rough;
                if (!be || !geom::outlinePoints(*be, 1e300, rough) || rough.size() < 2) {
                    ctx.message("That object cannot be used as a clipping boundary");
                    co_return;
                }
                double x0 = rough[0].x, y0 = rough[0].y, x1 = x0, y1 = y0;
                for (const Vec2& q : rough) {
                    x0 = std::min(x0, q.x), x1 = std::max(x1, q.x);
                    y0 = std::min(y0, q.y), y1 = std::max(y1, q.y);
                }
                const double diag = std::hypot(x1 - x0, y1 - y0);
                if (!(diag > 1e-9) || !geom::outlinePoints(*be, diag * 1e-4, world) ||
                    world.size() < 3) {
                    ctx.message("That object cannot be used as a clipping boundary");
                    co_return;
                }
                if (world.size() > 4096)
                    world.resize(4096);
                break;
            }
            if (first.status == cmd::Status::Keyword && first.keyword == "Polygonal") {
                for (;;) {
                    cmd::PointOptions po;
                    po.base = world.empty() ? std::nullopt : std::optional<Vec2>(world.back());
                    po.allowNone = world.size() >= 3;
                    if (!world.empty())
                        po.keywords = {"Undo"};
                    po.preview = [&world, &outline](Vec2 cursor, DisplayList& out) {
                        std::vector<Vec2> pts = world;
                        pts.push_back(cursor);
                        outline(pts, pts.size() >= 3, out);
                    };
                    const std::string prompt = world.empty() ? "Specify first point"
                                               : world.size() < 3
                                                   ? "Specify next point or [Undo]"
                                                   : "Specify next point or [Undo], Enter to close";
                    auto p = co_await ctx.getPoint(prompt, std::move(po));
                    if (p.status == cmd::Status::None)
                        break;
                    if (p.status == cmd::Status::Keyword) {
                        if (!world.empty())
                            world.pop_back();
                        continue;
                    }
                    if (!p.ok())
                        co_return;
                    world.push_back(p.point);
                }
                break;
            }
            if (!first.ok())
                co_return;
            const Vec2 a = first.point;
            cmd::PointOptions so;
            so.base = a;
            so.allowNone = false;
            so.preview = [a, &outline](Vec2 cursor, DisplayList& out) {
                outline({a, {cursor.x, a.y}, cursor, {a.x, cursor.y}}, true, out);
            };
            auto b = co_await ctx.getPoint("Specify opposite corner", std::move(so));
            if (!b.ok())
                co_return;
            if (std::abs(a.x - b.point.x) < 1e-9 || std::abs(a.y - b.point.y) < 1e-9) {
                ctx.message("The corners must differ in both directions");
                co_return;
            }
            world = {a, {b.point.x, a.y}, b.point, {a.x, b.point.y}};
            break;
        }
        const BlockDef* def = doc.block(ins->block);
        auto inv = blocks::instanceTransform(*ins, def->base).inverse();
        if (!inv) {
            ctx.message("The block is scaled to nothing; it cannot be clipped");
            co_return;
        }
        std::vector<Vec2> local;
        local.reserve(world.size());
        for (const Vec2& p : world)
            local.push_back(inv.value().apply(p));
        (void)ctx.tx().modify(id, [&](Entity& en) {
            auto* d = entityAs<InsertData>(en);
            d->clip = std::move(local);
            d->clipOn = true;
            d->clipInvert = invert;
        });
        ctx.setUndoLabel("Clip");
        const char* frameKey = pdf_ ? kPdfFrameKey : kXclipFrameKey;
        ctx.message("Clipped to a boundary of " + std::to_string(world.size()) + " points" +
                    (clipFrameMode(doc.header(), frameKey) == 0
                         ? std::string(". The boundary is hidden: ") +
                               (pdf_ ? "PDFFRAME" : "XCLIPFRAME") + " 2 shows it"
                         : std::string()));
    }

private:
    bool pdf_;
};

// XCLIPFRAME and PDFFRAME: 0 hides the clip boundary, 1 shows and plots it, 2 shows it on screen
// only. XCLIPFRAME is for blocks and xrefs, PDFFRAME for PDF underlays. Settings of the drawing.
class ClipFrameCommand final : public cmd::Command {
public:
    explicit ClipFrameCommand(bool pdf) : pdf_(pdf) {}
    std::string_view name() const override { return pdf_ ? "PDFFRAME" : "XCLIPFRAME"; }
    std::string_view description() const override {
        return pdf_ ? "Show, hide or plot the clip boundary of PDF underlays"
                    : "Show, hide or plot the clip boundary of blocks and xrefs";
    }

    cmd::Task run(cmd::CommandContext& ctx) override {
        const char* key = pdf_ ? kPdfFrameKey : kXclipFrameKey;
        const int now = clipFrameMode(ctx.doc().header(), key);
        cmd::IntOptions io;
        io.defaultValue = now;
        io.min = 0;
        io.max = 2;
        auto v = co_await ctx.getInteger(std::string(name()) +
                                             " 0 hidden, 1 shown and plotted, 2 shown not "
                                             "plotted <" +
                                             std::to_string(now) + ">",
                                         io);
        if (!v.ok())
            co_return;
        const std::string value = std::to_string(std::clamp<long long>(v.value, 0, 2));
        (void)ctx.tx().modifyHeader([&](Header& h) { h.custom[key] = value; });
        ctx.setUndoLabel(std::string(name()));
        ctx.message(std::string(name()) + " = " + value);
    }

private:
    bool pdf_;
};

// XREFFADE: how much lighter attached drawings look on screen (0 to 90 percent), so the lines
// of a reference read apart from the drawing's own. A setting of the drawing; plots ignore it.
class XrefFadeCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "XREFFADE"; }
    std::string_view description() const override {
        return "Set how much lighter attached drawings are shown on screen";
    }

    cmd::Task run(cmd::CommandContext& ctx) override {
        const int now = xrefFadePercent(ctx.doc().header());
        cmd::IntOptions io;
        io.defaultValue = now;
        io.min = 0;
        io.max = 90;
        auto v = co_await ctx.getInteger(
            "Lighten attached drawings by, 0 to 90 percent <" + std::to_string(now) + ">", io);
        if (!v.ok())
            co_return;
        const std::string value = std::to_string(std::clamp<long long>(v.value, 0, 90));
        (void)ctx.tx().modifyHeader([&](Header& h) { h.custom[kXrefFadeKey] = value; });
        ctx.setUndoLabel("XREFFADE");
        ctx.message("XREFFADE = " + value);
    }
};

}  // namespace

std::unique_ptr<cmd::Command> makeXrefFadeCommand() {
    return std::make_unique<XrefFadeCommand>();
}

std::unique_ptr<cmd::Command> makeClipFrameCommand(bool pdf) {
    return std::make_unique<ClipFrameCommand>(pdf);
}

std::unique_ptr<cmd::Command> makePdfClipCommand() {
    return std::make_unique<XClipCommand>(true);
}

std::unique_ptr<cmd::Command> makeXClipCommand() {
    return std::make_unique<XClipCommand>();
}

}  // namespace tbcad::block_cmds
