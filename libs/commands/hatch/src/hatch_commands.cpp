#include <algorithm>
#include <cmath>
#include <memory>

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
// not count. The host's spatial index answers the window query when it has one; otherwise the
// document is scanned.
std::vector<Candidate> viewCandidates(const Document& doc, const cmd::IHost& host) {
    const cmd::ViewState view = host.view();
    bool all = !(view.sizePx.x > 0 && view.sizePx.y > 0 && view.scale > 0);
    BBox2 window;
    if (!all) {
        Vec2 half{view.sizePx.x * view.scale / 2, view.sizePx.y * view.scale / 2};
        window.expand(view.center - half);
        window.expand(view.center + half);
    }
    bool hasBlocks = !doc.blocks().empty();
    std::vector<Candidate> out;
    auto consider = [&](const Entity& e, bool checkWindow) {
        if (!e.common.visible || !hasCurves(e))
            return;
        if (hasBlocks && doc.ownerBlock(e.common.id))
            return;
        if (checkWindow && !window.intersects(geom::bounds(e)))
            return;
        if (!doc.effective(e.common).live())
            return;
        out.push_back({e.common.id, &e});
    };
    if (!all) {
        if (auto ids = host.entitiesInWindow(window)) {
            for (EntityId id : *ids)
                if (const Entity* e = doc.find(id))
                    consider(*e, false);
            return out;
        }
    }
    doc.forEachEntity([&](const Entity& e) { consider(e, !all); });
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

// ---- Hover preview -------------------------------------------------------------------------

// The boundary graph of the view, rebuilt only when the drawing, the view or the gap changes, so
// moving the cursor costs one pick.
class HoverPreview {
public:
    void draw(cmd::CommandContext& ctx, const Params& p, Vec2 cursor, DisplayList& out) {
        refresh(ctx, p);
        if (!finder_ || !finder_->ok())
            return;
        auto b = finder_->find(cursor);
        if (!b.ok())
            return;
        const cmd::ViewState view = ctx.host().view();
        const Rgba line{255, 200, 40, 255};
        HatchData d;
        d.loops = b.value().loops;
        d.solid = true;
        Pattern solid;
        solid.solid = true;
        FillOptions fo;
        fo.tolerance = std::max(view.scale * 0.5, 1e-4);
        FillResult r = generateFill(d, solid, fo);
        if (!r.rings.empty()) {
            DisplayFill fill;
            fill.rings = std::move(r.rings);
            fill.style.color = Rgba{255, 200, 40, 70};
            out.items.emplace_back(std::move(fill));
        }
        for (const HatchLoop& l : d.loops) {
            HatchData one;
            one.loops.push_back(l);
            FillResult o = generateFill(one, solid, fo);
            for (auto& ring : o.rings) {
                DisplayPolyline pl;
                pl.pts = std::move(ring);
                pl.closed = true;
                pl.style.color = line;
                out.items.emplace_back(std::move(pl));
            }
        }
    }

private:
    void refresh(cmd::CommandContext& ctx, const Params& p) {
        const Document& doc = ctx.doc();
        const cmd::ViewState view = ctx.host().view();
        if (finder_ && revision_ == doc.revision() && gap_ == p.gap && center_ == view.center &&
            scale_ == view.scale && size_ == view.sizePx)
            return;
        revision_ = doc.revision();
        gap_ = p.gap;
        center_ = view.center;
        scale_ = view.scale;
        size_ = view.sizePx;
        BoundaryOptions bo;
        bo.gapTolerance = p.gap;
        finder_ = std::make_unique<BoundaryFinder>(viewCandidates(doc, ctx.host()), bo);
    }

    std::unique_ptr<BoundaryFinder> finder_;
    std::uint64_t revision_ = 0;
    double gap_ = 0;
    Vec2 center_{};
    double scale_ = 0;
    Vec2 size_{};
};

// ---- HATCH ---------------------------------------------------------------------------------

// HATCH fills with a pattern or a solid colour; GRADIENT is the same command with a colour
// gradient instead of a pattern.
class HatchCommand final : public cmd::Command {
public:
    explicit HatchCommand(bool gradient = false) : gradient_(gradient) {}
    std::string_view name() const override { return gradient_ ? "GRADIENT" : "HATCH"; }
    std::vector<std::string> defaultAliases() const override { return {gradient_ ? "GR" : "H"}; }
    std::string_view description() const override {
        return gradient_ ? "Fill an enclosed area with a colour gradient"
                         : "Fill an enclosed area with a pattern or a solid colour";
    }

    cmd::Task run(cmd::CommandContext& ctx) override {
        using cmd::Status;
        ctx.commitOnCancel();  // Esc ends the command but keeps the hatches already made
        Params p = loadParams(ctx.host());
        std::size_t made = 0;
        HoverPreview hover;
        for (;;) {
            cmd::PointOptions opt;
            opt.keywords = gradient_
                               ? std::vector<std::string>{"Objects", "Kind", "First", "Second",
                                                          "Angle",   "Gap",  "Link",  "Undo"}
                               : std::vector<std::string>{"Objects", "Pattern", "Scale", "Angle",
                                                          "Base",    "Gap",     "Link",  "Undo"};
            opt.preview = [&](Vec2 cursor, DisplayList& out) { hover.draw(ctx, p, cursor, out); };
            std::string what = gradient_ ? gradientSummary(p) : summary(p);
            auto r = co_await ctx.getPoint("Pick internal point (" + what + ")", opt);
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
                } else if (r.keyword == "Kind") {
                    cmd::KeywordOptions ko;
                    ko.keywords = {"Linear", "Cylinder"};
                    ko.defaultKeyword =
                        p.gradient.kind == GradientKind::Cylinder ? "Cylinder" : "Linear";
                    auto k = co_await ctx.getKeyword("Gradient kind", ko);
                    if (k.ok())
                        p.gradient.kind =
                            k.keyword == "Cylinder" ? GradientKind::Cylinder : GradientKind::Linear;
                } else if (r.keyword == "First" || r.keyword == "Second") {
                    bool first = r.keyword == "First";
                    cmd::StringOptions so;
                    so.defaultValue = colorText(first ? p.gradient.color1 : p.gradient.color2);
                    auto s2 = co_await ctx.getString(std::string(first ? "First" : "Second") +
                                                         " colour (index 1-255, r,g,b or #rrggbb)",
                                                     so);
                    if (s2.ok()) {
                        if (auto c = parseColor(s2.text))
                            (first ? p.gradient.color1 : p.gradient.color2) = *c;
                        else
                            ctx.message("Not a colour: \"" + cmd::trim(s2.text) + "\".");
                    }
                } else if (r.keyword == "Gap") {
                    cmd::RealOptions ro;
                    ro.defaultValue = p.gap;
                    ro.min = 0;
                    auto g = co_await ctx.getReal("Gap tolerance (0 = off)", ro);
                    if (g.ok())
                        p.gap = g.value;
                } else if (r.keyword == "Link") {
                    p.associative = !p.associative;
                    ctx.message(p.associative ? "Hatches follow their boundary objects."
                                              : "New hatches are not linked to their boundary.");
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
    bool gradient_;

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

    static BoundaryOptions options(const Params& p) {
        BoundaryOptions bo;
        bo.gapTolerance = p.gap;
        return bo;
    }

    static std::string gradientSummary(const Params& p) {
        return std::string(p.gradient.kind == GradientKind::Cylinder ? "cylinder " : "linear ") +
               colorText(p.gradient.color1) + " to " + colorText(p.gradient.color2) + ", angle " +
               number(p.angle * 180.0 / kPi) + (p.gap > 0 ? ", gap " + number(p.gap) : "") +
               (p.associative ? "" : ", not linked");
    }

    std::size_t add(cmd::CommandContext& ctx, const Params& p, const Boundary& b) const {
        const Pattern* pattern = PatternRegistry::find(gradient_ ? "SOLID" : p.pattern);
        if (!pattern)
            return 0;
        ctx.checkpoint();
        Entity hatch = makeHatchEntity(b, *pattern, p.scale, p.angle, p.base);
        if (!p.associative)
            entityAs<HatchData>(hatch)->sourceBoundary.clear();
        if (gradient_) {
            GradientData g = p.gradient;
            g.angle = p.angle;
            entityAs<HatchData>(hatch)->gradient = g;
        }
        auto id = ctx.tx().add(std::move(hatch));
        if (!id) {
            ctx.message(id.error().message);
            return 0;
        }
        ctx.message("Hatch created (" + std::to_string(b.loops.size()) + " " +
                    loopWord(b.loops.size()) + ").");
        return 1;
    }

    std::size_t hatchPoint(cmd::CommandContext& ctx, const Params& p, Vec2 at) {
        std::vector<Candidate> cands = viewCandidates(ctx.doc(), ctx.host());
        auto b = findBoundary(cands, at, options(p));
        if (!b.ok()) {
            ctx.message(b.error().message);
            return 0;
        }
        return add(ctx, p, b.value());
    }

    std::size_t hatchObjects(cmd::CommandContext& ctx, const Params& p,
                             const std::vector<EntityId>& ids) {
        auto bs = boundaryFromObjects(idCandidates(ctx.doc(), ids), options(p));
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
                        h.gradient.reset();
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
    (void)registry.add(std::make_unique<HatchCommand>(true));
    (void)registry.add(std::make_unique<HatchEditCommand>());
    registry.addCommitHook(
        [](Document& doc, Transaction& tx) { hatch::updateAssociativeHatches(doc, tx); });
}

}  // namespace tbcad::hatchcmd
