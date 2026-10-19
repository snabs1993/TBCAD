#include <algorithm>
#include <cmath>
#include <limits>

#include "tbcad/geom/geom.h"
#include "tbcad/transform/apply.h"
#include "tbcad/transform/grip_menu.h"
#include "tbcad/transform/grips.h"

namespace tbcad::transform {

namespace {

using cmd::CommandContext;
using cmd::PointOptions;
using cmd::Status;
using cmd::Task;

const char* promptFor(GripMode mode) {
    switch (mode) {
        case GripMode::Stretch:
            return "Specify stretch point";
        case GripMode::Move:
            return "Specify move point";
        case GripMode::Rotate:
            return "Specify rotation (cursor direction)";
        case GripMode::Scale:
            return "Specify scale (cursor distance from base)";
        case GripMode::Mirror:
            return "Specify second point of mirror line";
    }
    return "Specify point";
}

// Hidden command the UI starts when the user clicks a grip of the current selection:
// `GRIPEDIT x,y` (the selection is taken from the host, `x,y` picks the hot grip).
//
// Space / Enter cycle the mode stretch -> move -> rotate -> scale -> mirror; keywords pick one
// directly. "Copy" (Ctrl while dragging in the UI) leaves the original in place and keeps
// producing copies until Exit / Esc. "Base" moves the base point of rotate / scale / mirror.
class GripEditCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "GRIPEDIT"; }
    std::string_view description() const override { return {}; }  // hidden from the command list

    Task run(CommandContext& ctx) override {
        ctx.commitOnCancel();
        ctx.setUndoLabel("Grip edit");
        auto sel = co_await ctx.getSelection("Select objects");
        if (!sel.ok() || sel.ids.empty())
            co_return;
        auto pt = co_await ctx.getPoint("Specify hot grip", PointOptions{.allowNone = false});
        if (!pt.ok())
            co_return;

        const std::size_t limit = gripLimit(ctx.host());
        if (sel.ids.size() > limit) {
            ctx.message("Too many objects selected for grips (limit " + std::to_string(limit) +
                        ", setting grips.limit).");
            co_return;
        }
        std::vector<Grip> grips = collectGrips(ctx.doc(), sel.ids, limit);
        const double aperture = std::max(ctx.host().view().scale * 8.0, 1e-6);
        const Grip* found = hitGrip(grips, pt.point, aperture);
        if (!found) {
            ctx.message("No grip at that point.");
            co_return;
        }
        const Grip hot = *found;
        struct HotScope {  // the overlay draws the hot grip red while the command runs
            explicit HotScope(Vec2 p) { setHotGrip(p); }
            ~HotScope() { setHotGrip(std::nullopt); }
        } hotScope(hot.pos);

        const std::vector<GripAction> menu = gripActions(ctx.doc(), hot);
        GripMode mode = GripMode::Stretch;
        bool copy = false;
        Vec2 base = hot.pos;
        for (;;) {
            PointOptions o;
            o.base = (mode == GripMode::Stretch || mode == GripMode::Move) ? hot.pos : base;
            o.keywords = {"Base", "Copy",   "Undo",  "Exit",  "Stretch",
                          "Move", "Rotate", "Scale", "Mirror"};
            for (GripAction a : menu)
                if (a != GripAction::Stretch)
                    o.keywords.emplace_back(gripActionKeyword(a));
            o.preview = [&ctx, &sel, &grips, hot, &mode, &base](Vec2 c, DisplayList& out) {
                const double px = ctx.host().view().scale;
                if (mode == GripMode::Stretch) {
                    geom::DisplayContext dc;
                    dc.pixelSizeMm = px > 0 ? px : 1;
                    for (auto& [id, e] : stretchResults(ctx.doc(), grips, hot, c - hot.pos))
                        geom::buildDisplay(ctx.doc(), e, dc, out);
                    return;
                }
                if (auto t = modeTransform(mode, base, hot.pos, c))
                    buildPreview(ctx.doc(), sel.ids, *t, px, out);
            };
            std::string prompt = std::string(promptFor(mode)) + " or [Base/Copy/Undo/Exit] (" +
                                 gripModeName(mode) + (copy ? ", copy" : "") + ")";
            auto r = co_await ctx.getPoint(prompt, std::move(o));

            if (r.status == Status::Cancel)
                co_return;
            if (r.status == Status::None) {  // Enter / Space
                mode = nextGripMode(mode);
                continue;
            }
            if (r.status == Status::Keyword) {
                const std::string& k = r.keyword;
                if (k == "Exit")
                    co_return;
                if (k == "Copy") {
                    copy = !copy;
                    ctx.message(copy ? "Copy mode on." : "Copy mode off.");
                } else if (k == "Undo") {
                    if (!ctx.rollbackToCheckpoint())
                        ctx.message("Nothing to undo.");
                } else if (k == "Base") {
                    PointOptions bo;
                    auto b = co_await ctx.getPoint("Specify new base point", std::move(bo));
                    if (b.ok())
                        base = b.point;
                } else if (GripAction act; gripActionFromKeyword(k, act) &&
                                           act != GripAction::Stretch &&
                                           std::find(menu.begin(), menu.end(), act) != menu.end()) {
                    // A menu action of this grip (Add vertex, Lengthen, Radius ...).
                    Vec2 target = hot.pos;
                    if (gripActionNeedsPoint(act)) {
                        PointOptions ao;
                        ao.base = hot.pos;
                        ao.preview = [&ctx, hot, act](Vec2 c, DisplayList& out) {
                            const Entity* src = ctx.doc().find(hot.entity);
                            if (!src)
                                return;
                            auto res = applyGripAction(*src, hot.index, act, c);
                            if (!res)
                                return;
                            geom::DisplayContext dc;
                            const double px = ctx.host().view().scale;
                            dc.pixelSizeMm = px > 0 ? px : 1;
                            geom::buildDisplay(ctx.doc(), res.value(), dc, out);
                        };
                        auto pr = co_await ctx.getPoint(
                            std::string("Specify point for ") + gripActionLabel(act),
                            std::move(ao));
                        if (!pr.ok())
                            continue;
                        target = pr.point;
                    }
                    const Entity* src = ctx.doc().find(hot.entity);
                    if (!src)
                        co_return;
                    auto res = applyGripAction(*src, hot.index, act, target);
                    if (!res) {
                        ctx.message(std::string(gripActionLabel(act)) + ": " + res.error().message);
                        continue;
                    }
                    ctx.checkpoint();
                    Entity changed = std::move(res).value();
                    if (copy) {
                        (void)ctx.tx().add(std::move(changed));
                    } else {
                        (void)ctx.tx().modify(
                            hot.entity, [&](Entity& tgt) { tgt.data = std::move(changed.data); });
                    }
                    if (!copy)
                        co_return;
                } else if (k == "Stretch") {
                    mode = GripMode::Stretch;
                } else if (k == "Move") {
                    mode = GripMode::Move;
                } else if (k == "Rotate") {
                    mode = GripMode::Rotate;
                } else if (k == "Scale") {
                    mode = GripMode::Scale;
                } else if (k == "Mirror") {
                    mode = GripMode::Mirror;
                }
                continue;
            }
            if (!r.ok())
                co_return;

            // Apply.
            if (mode == GripMode::Mirror && distance(r.point, base) < 1e-12) {
                ctx.message("The two points of the mirror line must differ.");
                continue;
            }
            ctx.checkpoint();
            if (mode == GripMode::Stretch) {
                std::string err;
                auto results = stretchResults(ctx.doc(), grips, hot, r.point - hot.pos, &err);
                for (auto& [id, e] : results) {
                    if (copy) {
                        (void)ctx.tx().add(std::move(e));
                    } else {
                        Entity moved = std::move(e);
                        (void)ctx.tx().modify(
                            id, [&](Entity& tgt) { tgt.data = std::move(moved.data); });
                    }
                }
                if (!err.empty())
                    ctx.message("Could not stretch: " + err);
            } else {
                auto t = modeTransform(mode, base, hot.pos, r.point);
                if (!t) {
                    ctx.message("That point does not define a valid " +
                                std::string(gripModeName(mode)) + ".");
                    continue;
                }
                auto rep = applyTransform(ctx, sel.ids, *t, copy);
                if (rep.failed > 0)
                    ctx.message(std::to_string(rep.failed) +
                                " object(s) could not be transformed: " + rep.firstError);
            }
            if (!copy)
                co_return;
        }
    }
};

}  // namespace

// GRIPOBJLIMIT: grips are shown for a selection of at most this many objects (0: any number).
class GripObjLimitCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "GRIPOBJLIMIT"; }
    std::string_view description() const override {
        return "Most objects that still show grips when selected (0 = no limit)";
    }
    Task run(CommandContext& ctx) override {
        const std::size_t cur = gripLimit(ctx.host());
        const long long shown =
            cur == std::numeric_limits<std::size_t>::max() ? 0 : static_cast<long long>(cur);
        cmd::IntOptions io;
        io.defaultValue = shown;
        io.min = 0;
        io.max = 32767;
        auto r = co_await ctx.getInteger(
            "New value for GRIPOBJLIMIT <" + std::to_string(shown) + ">", io);
        if (!r.ok())
            co_return;
        ctx.host().setSetting("grips.limit", std::to_string(r.value));
        ctx.message("GRIPOBJLIMIT = " + std::to_string(r.value) + ".");
    }
};

void registerGripCommand(cmd::Registry& registry) {
    (void)registry.add(std::make_unique<GripEditCommand>());
    (void)registry.add(std::make_unique<GripObjLimitCommand>());
}

}  // namespace tbcad::transform
