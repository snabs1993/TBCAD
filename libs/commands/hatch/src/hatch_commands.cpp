#include <algorithm>
#include <cmath>

#include "hatch_params.h"
#include "tbcad/cmd/cmd.h"
#include "tbcad/geom/geom.h"
#include "tbcad/hatch/hatch.h"
#include "tbcad/hatchcmd/hatchcmd.h"

namespace tbcad::hatchcmd {

namespace {

using namespace tbcad::hatch;

// ---- Candidates ----------------------------------------------------------------------------

bool hasCurves(const Entity& e) {
    switch (kind(e)) {
        case EntityKind::Line:
        case EntityKind::Circle:
        case EntityKind::Arc:
        case EntityKind::Polyline:
        case EntityKind::Ellipse:
        case EntityKind::Spline:
            return true;
        default:
            return false;
    }
}

// The model-space entities that can bound a hatch and are inside the current view (every entity
// when the host has no viewport). Hidden objects and objects on layers that are off or frozen do
// not count. TODO(hatch): ask the host's spatial index instead of scanning the document, once
// IHost offers a window query (see the PR description).
std::vector<Candidate> viewCandidates(const Document& doc, const cmd::ViewState& view) {
    bool all = !(view.sizePx.x > 0 && view.sizePx.y > 0 && view.scale > 0);
    BBox2 window;
    if (!all) {
        Vec2 half{view.sizePx.x * view.scale / 2, view.sizePx.y * view.scale / 2};
        window.expand(view.center - half);
        window.expand(view.center + half);
    }
    bool hasBlocks = !doc.blocks().empty();
    std::vector<Candidate> out;
    doc.forEachEntity([&](const Entity& e) {
        if (!e.common.visible || !hasCurves(e))
            return;
        if (hasBlocks && doc.ownerBlock(e.common.id))
            return;
        if (!all && !window.intersects(geom::bounds(e)))
            return;
        if (!doc.effective(e.common.layer).visible())
            return;
        out.push_back({e.common.id, &e});
    });
    return out;
}

std::vector<Candidate> idCandidates(const Document& doc, const std::vector<EntityId>& ids) {
    std::vector<Candidate> out;
    for (EntityId id : ids)
        if (const Entity* e = doc.find(id); e && hasCurves(*e))
            out.push_back({id, e});
    return out;
}

const char* loopWord(std::size_t n) {
    return n == 1 ? "loop" : "loops";
}

// ---- HATCH ---------------------------------------------------------------------------------

class HatchCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "HATCH"; }
    std::vector<std::string> defaultAliases() const override { return {"H"}; }
    std::string_view description() const override {
        return "Fill an enclosed area with a pattern or a solid colour";
    }

    cmd::Task run(cmd::CommandContext& ctx) override {
        using cmd::Status;
        ctx.commitOnCancel();  // Esc ends the command but keeps the hatches already made
        Params p = loadParams(ctx.host());
        std::size_t made = 0;
        for (;;) {
            cmd::PointOptions opt;
            opt.keywords = {"Objects", "Pattern", "Scale", "Angle", "Base", "Undo"};
            auto r = co_await ctx.getPoint("Pick internal point (" + summary(p) + ")", opt);
            if (r.status == Status::Keyword) {
                if (r.keyword == "Pattern") {
                    cmd::StringOptions so;
                    so.defaultValue = p.pattern;
                    auto s = co_await ctx.getString("Pattern name, ? to list", so);
                    if (s.ok())
                        setPattern(ctx, p, s.text);
                } else if (r.keyword == "Scale") {
                    cmd::RealOptions ro;
                    ro.defaultValue = p.scale;
                    ro.min = 1e-6;
                    auto s = co_await ctx.getReal("Pattern scale", ro);
                    if (s.ok())
                        p.scale = s.value;
                } else if (r.keyword == "Angle") {
                    cmd::AngleOptions ao;
                    ao.defaultValue = p.angle;
                    auto a = co_await ctx.getAngle("Pattern angle", ao);
                    if (a.ok())
                        p.angle = a.value;
                } else if (r.keyword == "Base") {
                    auto b = co_await ctx.getPoint("Pattern origin");
                    if (b.ok())
                        p.base = b.point;
                } else if (r.keyword == "Undo") {
                    if (made > 0 && ctx.rollbackToCheckpoint()) {
                        --made;
                        ctx.message("Last hatch removed");
                    } else {
                        ctx.message("Nothing to undo");
                    }
                } else if (r.keyword == "Objects") {
                    cmd::SelectionOptions so;
                    so.allowPreselection = false;
                    so.filter = hasCurves;
                    auto sel = co_await ctx.getSelection("Select objects to hatch", so);
                    if (sel.ok() && !sel.ids.empty())
                        made += hatchObjects(ctx, p, sel.ids);
                }
                saveParams(ctx.host(), p);
                continue;
            }
            if (!r.ok())
                break;  // Enter or Esc
            made += hatchPoint(ctx, p, r.point);
        }
    }

private:
    static void setPattern(cmd::CommandContext& ctx, Params& p, const std::string& text) {
        std::string name = cmd::trim(text);
        if (name == "?") {
            std::string list;
            for (const std::string& n : PatternRegistry::names())
                list += (list.empty() ? "" : ", ") + n;
            ctx.message("Patterns: " + list);
        } else if (const Pattern* found = PatternRegistry::find(name)) {
            p.pattern = found->name;
        } else {
            ctx.message("Unknown pattern \"" + name + "\". Type ? to list the patterns.");
        }
    }

    static std::size_t add(cmd::CommandContext& ctx, const Params& p, const Boundary& b) {
        const Pattern* pattern = PatternRegistry::find(p.pattern);
        if (!pattern)
            return 0;
        ctx.checkpoint();
        auto id = ctx.tx().add(makeHatchEntity(b, *pattern, p.scale, p.angle, p.base));
        if (!id) {
            ctx.message(id.error().message);
            return 0;
        }
        ctx.message("Hatch created (" + std::to_string(b.loops.size()) + " " +
                    loopWord(b.loops.size()) + ").");
        return 1;
    }

    static std::size_t hatchPoint(cmd::CommandContext& ctx, const Params& p, Vec2 at) {
        std::vector<Candidate> cands = viewCandidates(ctx.doc(), ctx.host().view());
        auto b = findBoundary(cands, at);
        if (!b.ok()) {
            ctx.message(b.error().message);
            return 0;
        }
        return add(ctx, p, b.value());
    }

    static std::size_t hatchObjects(cmd::CommandContext& ctx, const Params& p,
                                    const std::vector<EntityId>& ids) {
        auto bs = boundaryFromObjects(idCandidates(ctx.doc(), ids));
        if (!bs.ok()) {
            ctx.message(bs.error().message);
            return 0;
        }
        std::size_t n = 0;
        for (const Boundary& b : bs.value())
            n += add(ctx, p, b);
        return n;
    }
};

// ---- HATCHEDIT -----------------------------------------------------------------------------

bool isHatch(const Entity& e) {
    return kind(e) == EntityKind::Hatch;
}

class HatchEditCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "HATCHEDIT"; }
    std::vector<std::string> defaultAliases() const override { return {"HE"}; }
    std::string_view description() const override {
        return "Change the pattern, scale, angle or origin of hatches";
    }

    cmd::Task run(cmd::CommandContext& ctx) override {
        using cmd::Status;
        cmd::SelectionOptions so;
        so.filter = isHatch;
        auto sel = co_await ctx.getSelection("Select hatch objects", so);
        if (sel.status == Status::Cancel)
            co_return;
        std::vector<EntityId> ids;
        for (EntityId id : sel.ids)
            if (const Entity* e = ctx.doc().find(id); e && isHatch(*e))
                ids.push_back(id);
        if (ids.empty()) {
            ctx.message("No hatch objects selected.");
            co_return;
        }
        ctx.setUndoLabel("Edit hatch");
        std::size_t changed = 0;
        for (;;) {
            cmd::KeywordOptions ko;
            ko.keywords = {"Pattern", "Scale", "Angle", "Base", "Done"};
            ko.defaultKeyword = "Done";
            auto k = co_await ctx.getKeyword("Hatch option", ko);
            if (!k.ok() || k.keyword == "Done")
                break;
            if (k.keyword == "Pattern") {
                cmd::StringOptions so2;
                so2.defaultValue = currentPattern(ctx, ids.front());
                auto s = co_await ctx.getString("Pattern name, ? to list", so2);
                if (!s.ok())
                    continue;
                std::string name = cmd::trim(s.text);
                const Pattern* pattern = PatternRegistry::find(name);
                if (name == "?") {
                    std::string list;
                    for (const std::string& n : PatternRegistry::names())
                        list += (list.empty() ? "" : ", ") + n;
                    ctx.message("Patterns: " + list);
                } else if (!pattern) {
                    ctx.message("Unknown pattern \"" + name + "\".");
                } else {
                    changed += apply(ctx, ids, [&](HatchData& h) {
                        h.pattern = pattern->name;
                        h.solid = pattern->solid;
                    });
                }
            } else if (k.keyword == "Scale") {
                cmd::RealOptions ro;
                ro.defaultValue = first(ctx, ids.front()).patternScale;
                ro.min = 1e-6;
                auto s = co_await ctx.getReal("Pattern scale", ro);
                if (s.ok())
                    changed += apply(ctx, ids, [&](HatchData& h) { h.patternScale = s.value; });
            } else if (k.keyword == "Angle") {
                cmd::AngleOptions ao;
                ao.defaultValue = first(ctx, ids.front()).patternAngle;
                auto a = co_await ctx.getAngle("Pattern angle", ao);
                if (a.ok())
                    changed += apply(ctx, ids, [&](HatchData& h) { h.patternAngle = a.value; });
            } else if (k.keyword == "Base") {
                auto b = co_await ctx.getPoint("Pattern origin");
                if (b.ok())
                    changed += apply(ctx, ids, [&](HatchData& h) { h.patternOrigin = b.point; });
            }
        }
        if (changed > 0)
            ctx.message(std::to_string(ids.size()) + " hatch object(s) updated.");
    }

private:
    static HatchData first(cmd::CommandContext& ctx, EntityId id) {
        const Entity* e = ctx.doc().find(id);
        const HatchData* h = e ? entityAs<HatchData>(*e) : nullptr;
        return h ? *h : HatchData{};
    }
    static std::string currentPattern(cmd::CommandContext& ctx, EntityId id) {
        std::string p = first(ctx, id).pattern;
        return p.empty() ? "SOLID" : p;
    }
    template <class Fn>
    static std::size_t apply(cmd::CommandContext& ctx, const std::vector<EntityId>& ids, Fn fn) {
        std::size_t n = 0;
        for (EntityId id : ids) {
            auto r = ctx.tx().modify(id, [&](Entity& e) {
                if (HatchData* h = entityAs<HatchData>(e))
                    fn(*h);
            });
            if (r)
                ++n;
        }
        return n;
    }
};

}  // namespace

void registerCommands(cmd::Registry& registry) {
    hatch::registerHatchDisplay();
    (void)registry.add(std::make_unique<HatchCommand>());
    (void)registry.add(std::make_unique<HatchEditCommand>());
}

}  // namespace tbcad::hatchcmd
