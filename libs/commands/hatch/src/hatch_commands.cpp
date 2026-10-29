#include <algorithm>
#include <cctype>
#include <cmath>
#include <deque>
#include <memory>
#include <optional>

#include "hatch_params.h"
#include "tbcad/blocks/blocks.h"
#include "tbcad/cmd/cmd.h"
#include "tbcad/core/groups.h"
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
// The window the search may use: the view, or the extents of the drawing without a viewport.
BBox2 searchWindow(const Document& doc, const cmd::IHost& host) {
    const cmd::ViewState view = host.view();
    BBox2 window;
    if (view.sizePx.x > 0 && view.sizePx.y > 0 && view.scale > 0) {
        Vec2 half{view.sizePx.x * view.scale / 2, view.sizePx.y * view.scale / 2};
        window.expand(view.center - half);
        window.expand(view.center + half);
    } else {
        doc.forEachEntity([&](const Entity& e) { window.merge(geom::bounds(e)); });
    }
    return window;
}

// World-space copies of the curves inside block references, which bound a hatch like any other
// object (walls and door blocks in a floor plan). Candidates point into it, so it must outlive
// the finder that uses them.
using CurveStore = std::deque<Entity>;

// Adds `e` to `out`; an Insert adds the visible curves it is made of, each with the Insert's id.
void addCandidate(const Document& doc, const Entity& e, CurveStore& store,
                  std::vector<Candidate>& out) {
    if (kind(e) != EntityKind::Insert) {
        if (hasCurves(e))
            out.push_back({e.common.id, &e});
        return;
    }
    for (Entity& part : blocks::snapGeometry(doc, e)) {
        if (!hasCurves(part) || !doc.effective(part.common).live())
            continue;
        store.push_back(std::move(part));
        out.push_back({e.common.id, &store.back()});
    }
}

// Entities that can bound a hatch and meet `window`. Clears `store` first.
std::vector<Candidate> windowCandidates(const Document& doc, const cmd::IHost& host,
                                        const BBox2& window, CurveStore& store) {
    store.clear();
    bool hasBlocks = !doc.blocks().empty();
    std::vector<Candidate> out;
    auto consider = [&](const Entity& e, bool checkWindow) {
        if (!e.common.visible || (!hasCurves(e) && kind(e) != EntityKind::Insert))
            return;
        if (hasBlocks && doc.ownerBlock(e.common.id))
            return;
        if (checkWindow &&
            !window.intersects(kind(e) == EntityKind::Insert ? blocks::insertBounds(doc, e)
                                                             : geom::bounds(e)))
            return;
        if (!doc.effective(e.common).live())
            return;
        addCandidate(doc, e, store, out);
    };
    if (auto ids = host.entitiesInWindow(window)) {
        for (EntityId id : *ids)
            if (const Entity* e = doc.find(id))
                consider(*e, false);
        return out;
    }
    doc.forEachEntity([&](const Entity& e) { consider(e, true); });
    return out;
}

std::vector<Candidate> viewCandidates(const Document& doc, const cmd::IHost& host,
                                      CurveStore& store) {
    return windowCandidates(doc, host, searchWindow(doc, host), store);
}

std::vector<Candidate> idCandidates(const Document& doc, const std::vector<EntityId>& ids,
                                    CurveStore& store) {
    std::vector<Candidate> out;
    for (EntityId id : ids)
        if (const Entity* e = doc.find(id))
            addCandidate(doc, *e, store, out);
    return out;
}

// True when a block reference is among the sources: an associative hatch re-derives its loops
// from the sources' own curves, which a block reference does not have.
bool usesBlock(const Document& doc, const std::vector<EntityId>& sources) {
    for (EntityId id : sources)
        if (const Entity* e = doc.find(id); e && kind(*e) == EntityKind::Insert)
            return true;
    return false;
}

const char* loopWord(std::size_t n) {
    return n == 1 ? "loop" : "loops";
}

// ---- Gaps -----------------------------------------------------------------------------------

// The free ends nearest to `at` (the likely reason there is no closed boundary there).
std::vector<Vec2> nearestOpenEnds(const BoundaryFinder& finder, Vec2 at, std::size_t count) {
    std::vector<Vec2> ends = finder.openEnds();
    std::sort(ends.begin(), ends.end(),
              [&](const Vec2& a, const Vec2& b) { return distance(a, at) < distance(b, at); });
    if (ends.size() > count)
        ends.resize(count);
    return ends;
}

// The smallest distance between two of the free ends: a Gap setting at least this big closes it.
std::optional<double> smallestGap(const std::vector<Vec2>& ends) {
    std::optional<double> best;
    for (std::size_t i = 0; i < ends.size(); ++i)
        for (std::size_t j = i + 1; j < ends.size(); ++j) {
            const double d = distance(ends[i], ends[j]);
            if (d > 0 && (!best || d < *best))
                best = d;
        }
    return best;
}

// Red rings on the free ends around the cursor, so a leak in the boundary can be seen.
void drawOpenEnds(const BoundaryFinder& finder, Vec2 at, double mmPerPx, DisplayList& out) {
    for (const Vec2& p : nearestOpenEnds(finder, at, 12)) {
        DisplayPolyline ring;
        const double r = 7 * mmPerPx;
        for (int i = 0; i < 16; ++i) {
            const double a = i * kPi / 8;
            ring.pts.push_back({p.x + r * std::cos(a), p.y + r * std::sin(a)});
        }
        ring.closed = true;
        ring.style.color = Rgba{255, 70, 70, 255};
        out.items.emplace_back(std::move(ring));
    }
}

// ---- Hover preview -------------------------------------------------------------------------

// The boundary graph of the view, rebuilt only when the drawing, the view or the gap changes, so
// moving the cursor costs one pick.
class HoverPreview {
public:
    void draw(cmd::CommandContext& ctx, const Params& p, Vec2 cursor, DisplayList& out) {
        refresh(ctx, p);
        const cmd::ViewState view = ctx.host().view();
        std::shared_ptr<BoundaryFinder> local;
        Result<Boundary> b = Error{};
        if (big_) {
            // Too much in view for one graph: search a window around the cursor instead.
            const Document& doc = ctx.doc();
            const cmd::IHost& host = ctx.host();
            BoundaryOptions bo;
            bo.gapTolerance = p.gap;
            NearBoundary nb = findBoundaryNear(
                [&](const BBox2& w) { return windowCandidates(doc, host, w, nearStore_); },
                searchWindow(doc, host), cursor, bo);
            b = std::move(nb.result);
            local = std::move(nb.finder);
            if (!local || !local->ok())
                return;
            if (!b.ok()) {
                drawOpenEnds(*local, cursor, view.scale, out);
                return;
            }
        } else {
            if (!finder_ || !finder_->ok())
                return;
            b = finder_->find(cursor);
            if (!b.ok()) {
                drawOpenEnds(*finder_, cursor, view.scale, out);
                return;
            }
        }
        const Document& pdoc = ctx.doc();
        applyIslandStyle(b.value(), p.island, [&](EntityId id) { return pdoc.find(id); });
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
        if ((finder_ || big_) && revision_ == doc.revision() && gap_ == p.gap &&
            center_ == view.center && scale_ == view.scale && size_ == view.sizePx)
            return;
        revision_ = doc.revision();
        gap_ = p.gap;
        center_ = view.center;
        scale_ = view.scale;
        size_ = view.sizePx;
        BoundaryOptions bo;
        bo.gapTolerance = p.gap;
        finder_.reset();
        std::vector<Candidate> cands = viewCandidates(doc, ctx.host(), store_);
        big_ = cands.size() > 4000;
        finder_ = big_ ? nullptr : std::make_unique<BoundaryFinder>(std::move(cands), bo);
    }

    CurveStore store_;      // block curves the view finder points into
    CurveStore nearStore_;  // the same for a search around the cursor
    std::unique_ptr<BoundaryFinder> finder_;
    bool big_ = false;
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
    std::vector<std::string> defaultAliases() const override {
        return gradient_ ? std::vector<std::string>{"GR"}
                         : std::vector<std::string>{"H", "BHATCH", "-HATCH"};
    }
    std::string_view description() const override {
        return gradient_ ? "Fill an enclosed area with a colour gradient"
                         : "Fill an enclosed area with a pattern or a solid colour";
    }

    cmd::Task run(cmd::CommandContext& ctx) override {
        using cmd::Status;
        ctx.commitOnCancel();  // Esc ends the command but keeps the hatches already made
        Params p = loadParams(ctx.host());
        Params seen = p;  // the saved settings p last agreed with
        if (ctx.doc().header().custom.count(kHpOriginKey))
            p.base = hatchOrigin(ctx.doc().header());  // HPORIGIN: where every new hatch starts
        // The Hatch panel saves a pattern, scale... picked while this command waits: take it, so
        // the next area gets what the panel shows (test.8: BRICK picked, LINE45 made).
        auto takePanelEdits = [&] {
            Params now = loadParams(ctx.host());
            if (sameSettings(now, seen))
                return;
            const Vec2 base = p.base;
            p = now;
            if (now.base == seen.base)
                p.base = base;  // the origin was not touched: keep HPORIGIN or Base
            seen = std::move(now);
        };
        last_.reset();  // each run starts a new hatch
        lastSig_.clear();
        ctx.host().setSetting("ui.showPanel", "hatch");  // like AutoCAD's contextual tab
        std::size_t made = 0;
        HoverPreview hover;
        for (;;) {
            takePanelEdits();
            cmd::PointOptions opt;
            opt.keywords =
                gradient_
                    ? std::vector<std::string>{"Objects",      "Kind",     "First",    "Second",
                                               "One",          "Centered", "Angle",    "Gap",
                                               "Island",       "Link",     "SEparate", "DRaworder",
                                               "Transparency", "Undo"}
                    : std::vector<std::string>{"Objects",      "Pattern",   "Scale",      "Angle",
                                               "Base",         "Gap",       "Island",     "Link",
                                               "SEparate",     "DRaworder", "ANnotative", "DOuble",
                                               "Transparency", "Undo"};
            opt.preview = [&](Vec2 cursor, DisplayList& out) {
                takePanelEdits();
                hover.draw(ctx, p, cursor, out);
            };
            opt.snap = false;  // a snapped pick would sit on the boundary
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
                    if (s.ok()) {
                        p.scale = s.value;
                        p.scaleAuto = false;
                    }
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
                    ko.keywords = kGradientKeywords;
                    ko.defaultKeyword = gradientKeyword(p.gradient.kind);
                    auto k = co_await ctx.getKeyword(
                        "Gradient kind [Linear/Cylinder/INVCylinder/Spherical/INVSpherical/"
                        "Hemispherical/INVHemispherical/CUrved/INVCUrved] <" +
                            std::string(gradientKeyword(p.gradient.kind)) + ">",
                        ko);
                    if (k.ok())
                        p.gradient.kind = gradientFromKeyword(k.keyword);
                } else if (r.keyword == "One") {
                    cmd::KeywordOptions ko;
                    ko.keywords = {"Yes", "No"};
                    ko.defaultKeyword = p.gradient.oneColor ? "Yes" : "No";
                    auto k = co_await ctx.getKeyword(std::string("One colour gradient [Yes/No] <") +
                                                         (p.gradient.oneColor ? "Yes" : "No") + ">",
                                                     ko);
                    if (k.ok())
                        p.gradient.oneColor = k.keyword == "Yes";
                    if (p.gradient.oneColor) {
                        cmd::RealOptions ro;
                        ro.defaultValue = p.gradient.tint;
                        ro.min = 0;
                        ro.max = 1;
                        auto t = co_await ctx.getReal(
                            "Shade to tint (0 black, 0.5 the colour, 1 white)", ro);
                        if (t.ok())
                            p.gradient.tint = t.value;
                    }
                } else if (r.keyword == "Centered") {
                    cmd::RealOptions ro;
                    ro.defaultValue = p.gradient.shift;
                    ro.min = 0;
                    ro.max = 1;
                    auto t = co_await ctx.getReal(
                        "Shift of the gradient's middle (0 = centred, AutoCAD uses 0.25)", ro);
                    if (t.ok())
                        p.gradient.shift = t.value;
                } else if (r.keyword == "SEparate") {
                    p.separate = !p.separate;
                    ctx.message(p.separate ? "Each area picked makes its own hatch."
                                           : "The areas picked in one HATCH make one hatch.");
                } else if (r.keyword == "DRaworder") {
                    cmd::KeywordOptions ko;
                    ko.keywords = {"None", "Back", "Front", "BEhind", "InFront"};
                    ko.defaultKeyword = orderKeyword(p.order);
                    auto k = co_await ctx.getKeyword(
                        "Draw order of new hatches [None/Back/Front/BEhind boundary/InFront of "
                        "boundary] <" +
                            std::string(orderKeyword(p.order)) + ">",
                        ko);
                    if (k.ok())
                        p.order = k.keyword == "None"      ? Params::Order::None
                                  : k.keyword == "Back"    ? Params::Order::Back
                                  : k.keyword == "Front"   ? Params::Order::Front
                                  : k.keyword == "InFront" ? Params::Order::InFront
                                                           : Params::Order::Behind;
                } else if (r.keyword == "ANnotative") {
                    p.annotative = !p.annotative;
                    ctx.message(p.annotative
                                    ? "New hatches are annotative: the scale is the size on paper."
                                    : "New hatches are not annotative.");
                } else if (r.keyword == "DOuble") {
                    p.doubled = !p.doubled;
                    ctx.message(p.doubled ? "The pattern is drawn twice, crossed."
                                          : "The pattern is drawn once.");
                } else if (r.keyword == "Transparency") {
                    cmd::StringOptions so;
                    so.defaultValue = transparencyText(p.transparency);
                    auto t = co_await ctx.getString("Transparency (ByLayer, ByBlock or 0-90)", so);
                    if (t.ok()) {
                        const std::string v = cmd::toUpperAscii(cmd::trim(t.text));
                        double d = 0;
                        if (v == "BYLAYER")
                            p.transparency = kTransparencyByLayer;
                        else if (v == "BYBLOCK")
                            p.transparency = kTransparencyByBlock;
                        else if (parseNumber(v, d) && d >= 0 && d <= 90 && d == std::floor(d))
                            p.transparency = static_cast<std::int8_t>(d);
                        else
                            ctx.message("Transparency is ByLayer, ByBlock or 0 to 90.");
                    }
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
                } else if (r.keyword == "Island") {
                    cmd::KeywordOptions ko;
                    ko.keywords = {"Normal", "Outer", "Ignore"};
                    ko.defaultKeyword = p.island == hatch::IslandStyle::Outer    ? "Outer"
                                        : p.island == hatch::IslandStyle::Ignore ? "Ignore"
                                                                                 : "Normal";
                    auto k = co_await ctx.getKeyword(
                        "Island detection [Normal/Outer/Ignore] <" +
                            std::string(ko.defaultKeyword.value_or("Normal")) + ">",
                        std::move(ko));
                    if (k.ok())
                        p.island = k.keyword == "Outer"    ? hatch::IslandStyle::Outer
                                   : k.keyword == "Ignore" ? hatch::IslandStyle::Ignore
                                                           : hatch::IslandStyle::Normal;
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
                    so.filter = [](const Entity& e) {
                        return hasCurves(e) || kind(e) == EntityKind::Insert;
                    };
                    auto sel = co_await ctx.getSelection("Select objects to hatch", so);
                    if (sel.ok() && !sel.ids.empty())
                        made += hatchObjects(ctx, p, sel.ids);
                }
                saveParams(ctx.host(), p);
                seen = loadParams(ctx.host());
                continue;
            }
            if (!r.ok())
                break;  // Enter or Esc
            takePanelEdits();
            made += hatchPoint(ctx, p, r.point);
        }
    }

private:
    bool gradient_;
    std::optional<EntityId> last_;  // the hatch this command made last (Separate off)
    std::string lastSig_;           // its settings: a change starts a new hatch

    static constexpr const char* orderKeyword(Params::Order o) {
        switch (o) {
            case Params::Order::None:
                return "None";
            case Params::Order::Back:
                return "Back";
            case Params::Order::Front:
                return "Front";
            case Params::Order::InFront:
                return "InFront";
            default:
                return "BEhind";
        }
    }

public:
    // HPDRAWORDER: puts a new hatch at the back, the front, or just behind / in front of the
    // objects that bound it.
    static void placeInOrder(cmd::CommandContext& ctx, EntityId id, const Params& p) {
        switch (p.order) {
            case Params::Order::None:
                return;
            case Params::Order::Back:
                (void)ctx.tx().sendToBack(id);
                return;
            case Params::Order::Front:
                (void)ctx.tx().bringToFront(id);
                return;
            default:
                break;
        }
        const Entity* e = ctx.doc().find(id);
        const HatchData* h = e ? entityAs<HatchData>(*e) : nullptr;
        if (!h || h->sourceBoundary.empty())
            return;
        std::vector<EntityId> order = ctx.doc().modelSpace();
        auto self = std::find(order.begin(), order.end(), id);
        if (self == order.end())
            return;  // not in model space
        order.erase(self);
        std::size_t first = order.size(), last = 0;
        bool any = false;
        for (std::size_t i = 0; i < order.size(); ++i)
            if (std::find(h->sourceBoundary.begin(), h->sourceBoundary.end(), order[i]) !=
                h->sourceBoundary.end()) {
                first = std::min(first, i);
                last = i;
                any = true;
            }
        if (!any)
            return;
        const std::size_t at = p.order == Params::Order::Behind ? first : last + 1;
        order.insert(order.begin() + static_cast<std::ptrdiff_t>(at), id);
        (void)ctx.tx().setDrawOrder(order);
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

    static BoundaryOptions options(const Params& p) {
        BoundaryOptions bo;
        bo.gapTolerance = p.gap;
        return bo;
    }

    static std::string gradientSummary(const Params& p) {
        std::string kind = gradientKeyword(p.gradient.kind);
        for (char& c : kind)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return kind + " " + colorText(p.gradient.color1) + " to " +
               (p.gradient.oneColor ? "tint " + number(p.gradient.tint)
                                    : colorText(p.gradient.color2)) +
               (p.gradient.shift > 0 ? ", shift " + number(p.gradient.shift) : "") + ", angle " +
               number(p.angle * 180.0 / kPi) + (p.gap > 0 ? ", gap " + number(p.gap) : "") +
               (p.associative ? "" : ", not linked");
    }

    // Scale 1 suits small shapes; a big area (a framing plan in millimetres) would turn solid
    // grey, so grow it in 1-2-5 steps to keep about 100 pattern units across the longer side.
    static double autoScale(const HatchData& h) {
        const BBox2 bb = hatchBounds(h);
        const double extent = std::max(bb.max.x - bb.min.x, bb.max.y - bb.min.y);
        const double raw = extent / 100.0;
        if (!(raw > 1.5) || !std::isfinite(raw))
            return 1;
        const double decade = std::pow(10.0, std::floor(std::log10(raw)));
        const double m = raw / decade;
        return decade * (m >= 5 ? 5 : m >= 2 ? 2 : 1);
    }

    std::size_t add(cmd::CommandContext& ctx, const Params& p, Boundary b) {
        const Document& sdoc = ctx.doc();
        applyIslandStyle(b, p.island, [&](EntityId id) { return sdoc.find(id); });
        const Pattern* pattern = PatternRegistry::find(gradient_ ? "SOLID" : p.pattern);
        if (!pattern)
            return 0;
        ctx.checkpoint();
        // Separate off: a later area joins the hatch this command made (one object, like
        // AutoCAD's HPSEPARATE 0).
        const std::string sig = gradient_ ? gradientSummary(p) : summary(p);
        if (!p.separate && last_ && sdoc.find(*last_) && sig == lastSig_) {
            const bool linked = p.associative && !usesBlock(sdoc, b.sources);
            const std::size_t loops = b.loops.size();
            auto r = ctx.tx().modify(*last_, [&](Entity& e) {
                HatchData* h = entityAs<HatchData>(e);
                if (!h)
                    return;
                h->loops.insert(h->loops.end(), b.loops.begin(), b.loops.end());
                if (linked && !h->sourceBoundary.empty()) {
                    for (EntityId s : b.sources)
                        if (std::find(h->sourceBoundary.begin(), h->sourceBoundary.end(), s) ==
                            h->sourceBoundary.end())
                            h->sourceBoundary.push_back(s);
                } else {
                    h->sourceBoundary.clear();
                }
            });
            if (!r) {
                ctx.message(r.error().message);
                return 0;
            }
            placeInOrder(ctx, *last_, p);
            ctx.message("Area added to the hatch (" + std::to_string(loops) + " " +
                        loopWord(loops) + ").");
            return 1;
        }
        // The angle typed is from the UCS X axis.
        const double angle = p.angle + ctx.host().ucsAngle();
        Entity hatch = makeHatchEntity(b, *pattern, p.scale, angle, p.base);
        if (p.scaleAuto)
            entityAs<HatchData>(hatch)->patternScale = autoScale(*entityAs<HatchData>(hatch));
        if (!p.associative || usesBlock(sdoc, b.sources))
            entityAs<HatchData>(hatch)->sourceBoundary.clear();
        if (gradient_) {
            GradientData g = p.gradient;
            g.angle = angle;
            if (g.oneColor)
                g.color2 = hatch::tintedColor(g.color1, g.tint);
            entityAs<HatchData>(hatch)->gradient = g;
        }
        entityAs<HatchData>(hatch)->doubled = p.doubled && !gradient_;
        applyCurrentProperties(ctx.doc().header(), hatch);
        if (p.transparency != kTransparencyByLayer)
            hatch.common.transparency = p.transparency;
        if (p.annotative && !gradient_)
            hatch.common.annotative = true;
        auto id = ctx.tx().add(std::move(hatch));
        if (!id) {
            ctx.message(id.error().message);
            return 0;
        }
        last_ = id.value();
        lastSig_ = sig;
        placeInOrder(ctx, id.value(), p);
        ctx.message("Hatch created (" + std::to_string(b.loops.size()) + " " +
                    loopWord(b.loops.size()) + ").");
        return 1;
    }

    std::size_t hatchPoint(cmd::CommandContext& ctx, const Params& p, Vec2 at) {
        const Document& doc = ctx.doc();
        const cmd::IHost& host = ctx.host();
        CurveStore store;
        NearBoundary near =
            findBoundaryNear([&](const BBox2& w) { return windowCandidates(doc, host, w, store); },
                             searchWindow(doc, host), at, options(p));
        auto& b = near.result;
        const BoundaryFinder& finder = *near.finder;
        if (!b.ok()) {
            ctx.message(b.error().message);
            if (finder.ok()) {
                if (auto gap = smallestGap(nearestOpenEnds(finder, at, 12)))
                    ctx.message("The outline is open near a free end, " + number(*gap) +
                                " apart from another. Set Gap to " + number(*gap * 1.01) +
                                " or more to close it.");
            }
            return 0;
        }
        return add(ctx, p, b.value());
    }

    std::size_t hatchObjects(cmd::CommandContext& ctx, const Params& p,
                             const std::vector<EntityId>& ids) {
        CurveStore store;
        auto bs = boundaryFromObjects(idCandidates(ctx.doc(), ids, store), options(p));
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
            // Show each change at once instead of when the command ends.
            if (changed > 0)
                ctx.checkpoint();
            cmd::KeywordOptions ko;
            ko.keywords = {"Pattern",      "Scale",    "Angle",     "Base",       "Gradient",
                           "DIsassociate", "SEparate", "DRaworder", "ANnotative", "DOuble",
                           "Transparency", "REcreate", "Done"};
            ko.defaultKeyword = "Done";
            auto k = co_await ctx.getKeyword(
                "Hatch option [Pattern/Scale/Angle/Base/Gradient/DIsassociate/SEparate/"
                "DRaworder/ANnotative/DOuble/Transparency/REcreate/Done] <Done>",
                ko);
            if (!k.ok() || k.keyword == "Done")
                break;
            if (k.keyword == "Gradient") {
                GradientData g = first(ctx, ids.front()).gradient.value_or(GradientData{});
                cmd::KeywordOptions gk;
                gk.keywords = kGradientKeywords;
                gk.keywords.push_back("Off");
                gk.defaultKeyword = gradientKeyword(g.kind);
                auto kk = co_await ctx.getKeyword(
                    "Gradient kind, or Off for a solid fill [Linear/Cylinder/INVCylinder/"
                    "Spherical/INVSpherical/Hemispherical/INVHemispherical/CUrved/INVCUrved/Off] "
                    "<" +
                        std::string(gradientKeyword(g.kind)) + ">",
                    gk);
                if (!kk.ok())
                    continue;
                if (kk.keyword == "Off") {
                    changed += apply(ctx, ids, [&](HatchData& h) { h.gradient.reset(); });
                    continue;
                }
                g.kind = gradientFromKeyword(kk.keyword);
                cmd::StringOptions c1;
                c1.defaultValue = colorText(g.color1);
                auto s1 =
                    co_await ctx.getString("First colour (index 1-255, r,g,b or #rrggbb)", c1);
                if (!s1.ok())
                    continue;
                if (auto c = parseColor(s1.text))
                    g.color1 = *c;
                cmd::StringOptions c2;
                c2.defaultValue = g.oneColor ? "Tint " + number(g.tint) : colorText(g.color2);
                auto s2 = co_await ctx.getString(
                    "Second colour, or T and a number 0-1 for a one-colour gradient", c2);
                if (!s2.ok())
                    continue;
                const std::string t2 = cmd::toUpperAscii(cmd::trim(s2.text));
                double tint = 0;
                const std::size_t digits = t2.find_first_not_of("TINT ");
                if (t2.rfind("T", 0) == 0 && digits != std::string::npos &&
                    parseNumber(t2.substr(digits), tint) && tint >= 0 && tint <= 1) {
                    g.oneColor = true;
                    g.tint = tint;
                    g.color2 = hatch::tintedColor(g.color1, tint);
                } else if (auto c = parseColor(s2.text)) {
                    g.oneColor = false;
                    g.color2 = *c;
                }
                cmd::RealOptions ro;
                ro.defaultValue = g.shift;
                ro.min = 0;
                ro.max = 1;
                auto sh = co_await ctx.getReal("Shift (0 = centred)", ro);
                if (sh.ok())
                    g.shift = sh.value;
                changed += apply(ctx, ids, [&](HatchData& h) {
                    const double angle = h.gradient ? h.gradient->angle : h.patternAngle;
                    h.gradient = g;
                    h.gradient->angle = angle;
                    h.solid = true;
                });
            } else if (k.keyword == "DIsassociate") {
                changed += apply(ctx, ids, [](HatchData& h) { h.sourceBoundary.clear(); });
                ctx.message("The hatches no longer follow their boundary objects.");
            } else if (k.keyword == "SEparate") {
                changed += separate(ctx, ids);
            } else if (k.keyword == "REcreate") {
                cmd::KeywordOptions yn;
                yn.keywords = {"Yes", "No"};
                yn.defaultKeyword = "No";
                auto a = co_await ctx.getKeyword(
                    "Associate the hatch with the new boundary [Yes/No] <No>", yn);
                if (!a.ok())
                    continue;
                changed += recreate(ctx, ids, a.keyword == "Yes");
            } else if (k.keyword == "DRaworder") {
                cmd::KeywordOptions dk;
                dk.keywords = {"Back", "Front", "BEhind", "InFront"};
                dk.defaultKeyword = "BEhind";
                auto d = co_await ctx.getKeyword(
                    "Draw order [Back/Front/BEhind boundary/InFront of boundary] <BEhind>", dk);
                if (!d.ok())
                    continue;
                Params order;
                order.order = d.keyword == "Back"      ? Params::Order::Back
                              : d.keyword == "Front"   ? Params::Order::Front
                              : d.keyword == "InFront" ? Params::Order::InFront
                                                       : Params::Order::Behind;
                for (EntityId id : ids)
                    HatchCommand::placeInOrder(ctx, id, order);
                ++changed;
            } else if (k.keyword == "ANnotative" || k.keyword == "DOuble") {
                const bool annot = k.keyword == "ANnotative";
                cmd::KeywordOptions yn;
                yn.keywords = {"Yes", "No"};
                const Entity* e0 = ctx.doc().find(ids.front());
                const bool now =
                    annot ? e0 && e0->common.annotative : first(ctx, ids.front()).doubled;
                yn.defaultKeyword = now ? "Yes" : "No";
                auto a = co_await ctx.getKeyword(std::string(annot ? "Annotative" : "Double") +
                                                     " [Yes/No] <" + (now ? "Yes" : "No") + ">",
                                                 yn);
                if (!a.ok())
                    continue;
                const bool on = a.keyword == "Yes";
                for (EntityId id : ids) {
                    auto r = ctx.tx().modify(id, [&](Entity& e) {
                        if (annot)
                            e.common.annotative = on;
                        else if (HatchData* h = entityAs<HatchData>(e))
                            h->doubled = on && !h->solid;
                    });
                    if (r)
                        ++changed;
                }
            } else if (k.keyword == "Transparency") {
                cmd::StringOptions ts;
                const Entity* e0 = ctx.doc().find(ids.front());
                ts.defaultValue =
                    transparencyText(e0 ? e0->common.transparency : kTransparencyByLayer);
                auto t = co_await ctx.getString("Transparency (ByLayer, ByBlock or 0-90)", ts);
                if (!t.ok())
                    continue;
                const std::string v = cmd::toUpperAscii(cmd::trim(t.text));
                double d = 0;
                std::int8_t value = 0;
                if (v == "BYLAYER")
                    value = kTransparencyByLayer;
                else if (v == "BYBLOCK")
                    value = kTransparencyByBlock;
                else if (parseNumber(v, d) && d >= 0 && d <= 90 && d == std::floor(d))
                    value = static_cast<std::int8_t>(d);
                else {
                    ctx.message("Transparency is ByLayer, ByBlock or 0 to 90.");
                    continue;
                }
                for (EntityId id : ids)
                    if (ctx.tx().modify(id, [&](Entity& e) { e.common.transparency = value; }))
                        ++changed;
            } else if (k.keyword == "Pattern") {
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
    // A closed polyline for every loop of each hatch, on the hatch's layer (HATCHEDIT REcreate).
    // With `associate` the hatch follows the new polylines, like one made from picked objects.
    static std::size_t recreate(cmd::CommandContext& ctx, const std::vector<EntityId>& ids,
                                bool associate) {
        std::size_t made = 0;
        for (EntityId id : ids) {
            const Entity* e = ctx.doc().find(id);
            const HatchData* h = e ? entityAs<HatchData>(*e) : nullptr;
            if (!h)
                continue;
            Entity proto = *e;
            const std::vector<HatchLoop> loops = h->loops;  // `h` points into the document
            std::vector<EntityId> created;
            for (const HatchLoop& loop : loops) {
                PolylineData pl = hatch::loopPolyline(loop);
                if (pl.v.size() < 2)
                    continue;
                Entity piece = proto;
                piece.data = std::move(pl);
                if (auto nid = ctx.tx().add(std::move(piece)))
                    created.push_back(nid.value());
            }
            made += created.size();
            if (associate && !created.empty())
                (void)ctx.tx().modify(
                    id, [&](Entity& x) { entityAs<HatchData>(x)->sourceBoundary = created; });
        }
        ctx.message(made == 0 ? "No boundary could be made."
                              : std::to_string(made) + " boundary polyline(s) created.");
        return made;
    }

    // Each area of a hatch becomes a hatch of its own (HATCHEDIT Separate).
    static std::size_t separate(cmd::CommandContext& ctx, std::vector<EntityId>& ids) {
        std::size_t made = 0;
        std::vector<EntityId> all;
        for (EntityId id : ids) {
            const Entity* e = ctx.doc().find(id);
            const HatchData* h = e ? entityAs<HatchData>(*e) : nullptr;
            if (!h) {
                continue;
            }
            std::vector<HatchData> parts = hatch::separateAreas(*h);
            all.push_back(id);
            if (parts.size() < 2)
                continue;
            const Entity copy = *e;
            (void)ctx.tx().modify(id, [&](Entity& x) { *entityAs<HatchData>(x) = parts[0]; });
            for (std::size_t i = 1; i < parts.size(); ++i) {
                Entity piece = copy;
                piece.data = parts[i];
                if (auto nid = ctx.tx().add(std::move(piece))) {
                    all.push_back(nid.value());
                    ++made;
                }
            }
        }
        if (made == 0) {
            ctx.message("Each selected hatch has one area only.");
            return 0;
        }
        ids = all;
        ctx.message(std::to_string(made) + " more hatch(es) made from the separate areas.");
        return made;
    }

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

// ---- SUPERHATCH ----------------------------------------------------------------------------

bool insideRings(const std::vector<std::vector<Vec2>>& rings, Vec2 pt) {
    bool in = false;  // even-odd: islands inside the outline are taken away
    for (const auto& r : rings)
        for (std::size_t i = 0, j = r.size() - 1; i < r.size(); j = i++)
            if ((r[i].y > pt.y) != (r[j].y > pt.y) &&
                pt.x < (r[j].x - r[i].x) * (pt.y - r[i].y) / (r[j].y - r[i].y) + r[i].x)
                in = !in;
    return in;
}

// SUPERHATCH fills an enclosed area with copies of a block (or an attached xref, which is a
// block too) laid out on a grid as big as the block, like a hatch pattern made of that block.
// Only copies that lie wholly inside the area are made. The grid starts at HPORIGIN.
class SuperHatchCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "SUPERHATCH"; }
    std::string_view description() const override {
        return "Fill an enclosed area with copies of a block or xref";
    }

    cmd::Task run(cmd::CommandContext& ctx) override {
        using cmd::Status;
        ctx.commitOnCancel();
        Params p = loadParams(ctx.host());
        std::string blockName;
        if (auto v = ctx.host().setting("hatch.superBlock"))
            blockName = *v;
        double scale = 1;
        if (auto v = ctx.host().setting("hatch.superScale");
            !v || !parseNumber(*v, scale) || !(scale > 0))
            scale = 1;
        const bool haveOrigin = ctx.doc().header().custom.count(kHpOriginKey) != 0;
        const Vec2 origin = haveOrigin ? hatchOrigin(ctx.doc().header()) : p.base;
        std::size_t made = 0;
        for (;;) {
            cmd::PointOptions opt;
            opt.keywords = {"Block", "Scale"};
            opt.snap = false;
            auto r =
                co_await ctx.getPoint("Pick internal point (block " +
                                          (blockName.empty() ? std::string("none") : blockName) +
                                          ", scale " + number(scale) + ")",
                                      opt);
            if (r.status == Status::Keyword) {
                if (r.keyword == "Block") {
                    cmd::StringOptions so;
                    so.defaultValue = blockName;
                    auto s = co_await ctx.getString("Block or xref name", so);
                    if (s.ok()) {
                        const std::string n = cmd::trim(s.text);
                        if (ctx.doc().blockByName(n)) {
                            blockName = n;
                            ctx.host().setSetting("hatch.superBlock", n);
                        } else {
                            ctx.message("No block or xref named \"" + n + "\".");
                        }
                    }
                } else if (r.keyword == "Scale") {
                    cmd::RealOptions ro;
                    ro.defaultValue = scale;
                    ro.min = 1e-6;
                    auto s = co_await ctx.getReal("Scale of the block", ro);
                    if (s.ok()) {
                        scale = s.value;
                        ctx.host().setSetting("hatch.superScale", number(scale));
                    }
                }
                continue;
            }
            if (!r.ok())
                break;
            if (blockName.empty()) {
                ctx.message("Choose a Block first.");
                continue;
            }
            made += fill(ctx, p, r.point, blockName, scale, origin);
        }
        if (made)
            ctx.message(std::to_string(made) + " block copies placed.");
    }

private:
    static std::size_t fill(cmd::CommandContext& ctx, const Params& p, Vec2 at,
                            const std::string& blockName, double scale, Vec2 origin) {
        const Document& doc = ctx.doc();
        const cmd::IHost& host = ctx.host();
        const BlockDef* def = doc.blockByName(blockName);
        if (!def) {
            ctx.message("No block or xref named \"" + blockName + "\".");
            return 0;
        }
        const BBox2 bb = blocks::blockBounds(doc, def->id);
        if (bb.empty() || !(bb.max.x - bb.min.x > 1e-9) || !(bb.max.y - bb.min.y > 1e-9)) {
            ctx.message("The block is empty or has no width or height.");
            return 0;
        }
        CurveStore store;
        NearBoundary near =
            findBoundaryNear([&](const BBox2& w) { return windowCandidates(doc, host, w, store); },
                             searchWindow(doc, host), at, options(p));
        if (!near.result.ok()) {
            ctx.message(near.result.error().message);
            return 0;
        }
        const Pattern* solid = PatternRegistry::find("SOLID");
        if (!solid)
            return 0;
        Entity area = makeHatchEntity(near.result.value(), *solid, 1, 0, {});
        const auto rings = hatch::loopRings(*entityAs<HatchData>(area), 0.01);
        const BBox2 ab = hatchBounds(*entityAs<HatchData>(area));
        const double w = (bb.max.x - bb.min.x) * scale, h = (bb.max.y - bb.min.y) * scale;
        constexpr std::size_t kMax = 20000;
        if (((ab.max.x - ab.min.x) / w + 1) * ((ab.max.y - ab.min.y) / h + 1) > 4.0 * kMax) {
            ctx.message("The block is too small for this area (more than " + std::to_string(kMax) +
                        " copies). Raise Scale.");
            return 0;
        }
        const auto first = [&](double lo, double org, double step) {
            return org + std::floor((lo - org) / step) * step;
        };
        std::vector<Entity> copies;
        for (double y = first(ab.min.y, origin.y, h); y < ab.max.y; y += h)
            for (double x = first(ab.min.x, origin.x, w); x < ab.max.x; x += w) {
                if (!insideRings(rings, {x + w * 0.02, y + h * 0.02}) ||
                    !insideRings(rings, {x + w * 0.98, y + h * 0.02}) ||
                    !insideRings(rings, {x + w * 0.98, y + h * 0.98}) ||
                    !insideRings(rings, {x + w * 0.02, y + h * 0.98}))
                    continue;
                Entity e;
                InsertData ins;
                ins.block = def->id;
                ins.scale = {scale, scale};
                ins.position = {x - bb.min.x * scale, y - bb.min.y * scale};
                e.data = std::move(ins);
                applyCurrentProperties(doc.header(), e);
                copies.push_back(std::move(e));
                if (copies.size() > kMax) {
                    ctx.message("More than " + std::to_string(kMax) + " copies: raise Scale.");
                    return 0;
                }
            }
        if (copies.empty()) {
            ctx.message("No whole copy of the block fits in this area. Lower Scale.");
            return 0;
        }
        ctx.checkpoint();
        std::size_t n = 0;
        for (Entity& e : copies)
            if (ctx.tx().add(std::move(e)))
                ++n;
        return n;
    }

    static BoundaryOptions options(const Params& p) {
        BoundaryOptions bo;
        bo.gapTolerance = p.gap;
        return bo;
    }
};

}  // namespace

void registerCommands(cmd::Registry& registry) {
    hatch::registerHatchDisplay();
    (void)registry.add(std::make_unique<HatchCommand>());
    (void)registry.add(std::make_unique<HatchCommand>(true));
    (void)registry.add(std::make_unique<HatchEditCommand>());
    (void)registry.add(std::make_unique<SuperHatchCommand>());
    registry.addCommitHook(
        [](Document& doc, Transaction& tx) { hatch::updateAssociativeHatches(doc, tx); });
}

}  // namespace tbcad::hatchcmd
