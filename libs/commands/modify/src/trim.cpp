#include <algorithm>
#include <cmath>
#include <deque>
#include <limits>
#include <map>
#include <unordered_set>

#include "commands.h"
#include "tbcad/blocks/blocks.h"
#include "util.h"

namespace tbcad::modifycmd {

namespace {

constexpr const char* kEdgeExtendKey = "trim.edgeextend";
constexpr const char* kQuickKey = "trim.quick";  // Quick mode: every object is an edge

struct EdgeSet {
    std::vector<EntityId> ids{};
    bool all = true;  // every visible curve is an edge
};

// One candidate edge: skips the target and what cannot cut; inserts give their curves, kept alive
// in `owned`.
void considerEdge(const Document& doc, EntityId id, EntityId target, std::vector<const Entity*>& out,
                  std::deque<Entity>& owned) {
    if (id == target)
        return;
    const Entity* e = doc.find(id);
    if (!e || !isUsableBoundary(doc, *e))
        return;
    if (kind(*e) == EntityKind::Insert) {
        for (Entity& part : blocks::snapGeometry(doc, *e))
            if (modify::isCurveEntity(part))
                out.push_back(&owned.emplace_back(std::move(part)));
        return;
    }
    out.push_back(e);
}

// The end of a line or open straight-sided polyline that EXTEND would move, as a ray going on from
// it. Other objects (arcs, polylines with bulges) have no simple ray.
struct ExtendRay {
    Vec2 origin{}, dir{};
};
std::optional<ExtendRay> extensionRay(const Entity& e, Vec2 pick) {
    auto make = [](Vec2 end, Vec2 from) -> std::optional<ExtendRay> {
        const Vec2 d = end - from;
        const double len = length(d);
        if (!(len > 1e-12))
            return std::nullopt;
        return ExtendRay{end, d / len};
    };
    if (const auto* l = entityAs<LineData>(e))
        return distance(pick, l->b) < distance(pick, l->a) ? make(l->b, l->a) : make(l->a, l->b);
    if (const auto* p = entityAs<PolylineData>(e)) {
        const std::size_t n = p->v.size();
        if (p->closed || n < 2)
            return std::nullopt;
        if (distance(pick, p->v.back().p) < distance(pick, p->v.front().p))
            return p->v[n - 2].bulge == 0 ? make(p->v[n - 1].p, p->v[n - 2].p) : std::nullopt;
        return p->v[0].bulge == 0 ? make(p->v[0].p, p->v[1].p) : std::nullopt;
    }
    return std::nullopt;
}

// How far the end at `origin` moved; infinity when the other end moved instead or the object has no
// two ends.
double movedBy(const Entity& before, const Entity& after, Vec2 origin) {
    auto ends = [](const Entity& e) -> std::optional<std::pair<Vec2, Vec2>> {
        if (const auto* l = entityAs<LineData>(e))
            return std::make_pair(l->a, l->b);
        if (const auto* p = entityAs<PolylineData>(e); p && !p->v.empty())
            return std::make_pair(p->v.front().p, p->v.back().p);
        return std::nullopt;
    };
    const auto a = ends(before), b = ends(after);
    constexpr double inf = std::numeric_limits<double>::infinity();
    if (!a || !b)
        return inf;
    const bool first = distance(a->first, origin) < 1e-9;
    const bool last = distance(a->second, origin) < 1e-9;
    if (first == last)
        return inf;
    const double other = first ? distance(a->second, b->second) : distance(a->first, b->first);
    if (other > 1e-9)
        return inf;
    return first ? distance(a->first, b->first) : distance(a->second, b->second);
}

// Edges for one operation. Trimming only needs edges that can touch the target, extending also
// looks at edges far from it.
// Curves taken out of block references are kept alive in `owned`.
std::vector<const Entity*> collectEdges(const Document& doc, const EdgeSet& set, EntityId target,
                                        bool trimming, bool extendEdges, std::deque<Entity>& owned,
                                        const cmd::IHost* host = nullptr) {
    std::vector<const Entity*> out;
    const Entity* t = doc.find(target);
    BBox2 box;
    const bool prune = trimming && !extendEdges && t;
    if (prune)
        box = geom::bounds(*t, doc).inflated(1e-6);
    auto consider = [&](EntityId id) {
        if (prune) {
            const Entity* e = doc.find(id);
            if (e && isUsableBoundary(doc, *e) && !geom::bounds(*e, doc).intersects(box))
                return;
        }
        considerEdge(doc, id, target, out, owned);
    };
    if (set.all) {
        // Trimming only needs the edges near the target: the spatial index finds them without
        // looking at every object (a click would otherwise cost time in proportion to the drawing).
        std::optional<std::vector<EntityId>> near;
        if (prune && host)
            near = host->entitiesInWindow(box);
        if (near) {
            std::sort(near->begin(), near->end(),
                      [](EntityId a, EntityId b) { return a.value < b.value; });
            for (EntityId id : *near)
                consider(id);
        } else {
            for (EntityId id : doc.modelSpace())
                consider(id);
        }
    } else {
        for (EntityId id : set.ids)
            consider(id);
    }
    return out;
}

// Trims or extends one entity at a pick. Returns the ids standing for it afterwards.
std::optional<std::vector<EntityId>> doOne(cmd::CommandContext& ctx, EntityId id, Vec2 pick,
                                           bool trimming, const EdgeSet& set,
                                           bool checkpoint = true) {
    const Entity* target = ctx.doc().find(id);
    if (!target)
        return std::nullopt;
    modify::TrimOptions opt;
    opt.extendEdges = getFlag(ctx.host(), kEdgeExtendKey, false);
    std::deque<Entity> owned;
    Result<modify::EntityEdit> edit = Error{ErrorCode::NotFound, ""};
    if (!trimming && set.all && !opt.extendEdges) {
        // The nearest boundary ahead is looked for along the ray the end moves on, in squares
        // that follow it and a span that grows by steps, so a click or a fence over thousands of
        // objects tests each only against the objects near its own path. A hit inside the span
        // already searched is the nearest one.
        const BBox2 world = ctx.host().viewExtents();
        const auto ray = extensionRay(*target, pick);
        if (ray && !world.empty()) {
            const BBox2 own = geom::bounds(*target, ctx.doc());
            const double size = std::max({own.width(), own.height(), 1.0});
            const double maxT = length(world.max - world.min) + distance(ray->origin, world.center());
            std::unordered_set<EntityId> seen;
            std::vector<const Entity*> edges;
            double t0 = 0;
            for (double t1 = size * 4;; t1 *= 4) {
                t1 = std::min(t1, maxT + 1);
                const double q = std::max(size * 0.5, (t1 - t0) / 1024.0);
                bool indexed = true;
                for (double t = t0; t < t1 && indexed; t += q) {
                    const Vec2 c = ray->origin + ray->dir * (t + q * 0.5);
                    const double r = q * 0.75;
                    const auto ids = ctx.host().entitiesInWindow(BBox2{{c.x - r, c.y - r}, {c.x + r, c.y + r}});
                    if (!ids) {
                        indexed = false;
                        break;
                    }
                    for (EntityId e : *ids)
                        if (seen.insert(e).second)
                            considerEdge(ctx.doc(), e, id, edges, owned);
                }
                if (!indexed) {
                    edges.clear();
                    owned.clear();
                    edges = collectEdges(ctx.doc(), set, id, false, false, owned, &ctx.host());
                    edit = modify::extendEntity(*target, pick, edges, opt);
                    break;
                }
                edit = modify::extendEntity(*target, pick, edges, opt);
                double moved = 0;
                if (edit && edit.value().modified)
                    moved = movedBy(*target, *edit.value().modified, ray->origin);
                if (std::isinf(moved)) {  // not the end the ray follows: look at everything
                    edges = collectEdges(ctx.doc(), set, id, false, false, owned, &ctx.host());
                    edit = modify::extendEntity(*target, pick, edges, opt);
                    break;
                }
                if ((edit && moved <= t1 - q * 0.25) || t1 > maxT)
                    break;
                t0 = t1;
            }
        } else {
            auto edges = collectEdges(ctx.doc(), set, id, false, false, owned, &ctx.host());
            edit = modify::extendEntity(*target, pick, edges, opt);
        }
    } else {
        auto edges =
            collectEdges(ctx.doc(), set, id, trimming, opt.extendEdges, owned, &ctx.host());
        edit = trimming ? modify::trimEntity(*target, pick, edges, opt)
                        : modify::extendEntity(*target, pick, edges, opt);
    }
    if (!edit) {
        ctx.message(edit.error().message);
        return std::nullopt;
    }
    if (checkpoint)
        ctx.checkpoint();
    auto ids = applyEdit(ctx.tx(), id, std::move(edit.value()));
    if (!ids) {
        ctx.message(ids.error().message);
        return std::nullopt;
    }
    return std::move(ids.value());
}

cmd::Task readPoints(cmd::CommandContext& ctx, const char* first, const char* next, bool rectangle,
                     std::vector<Vec2>& out, bool& ok) {
    ok = false;
    auto a = co_await ctx.getPoint(first);
    if (!a.ok())
        co_return;
    out.push_back(a.point);
    for (;;) {
        cmd::PointOptions po;
        po.base = out.back();
        po.preview = [pts = out, rectangle](Vec2 cursor, DisplayList& list) {
            DisplayPolyline pl;
            if (rectangle) {
                pl.pts = {
                    pts.front(), {cursor.x, pts.front().y}, cursor, {pts.front().x, cursor.y}};
                pl.closed = true;
            } else {
                pl.pts = pts;
                pl.pts.push_back(cursor);
            }
            list.items.emplace_back(std::move(pl));
        };
        auto p = co_await ctx.getPoint(next, po);
        if (p.status == cmd::Status::Cancel)
            co_return;
        if (!p.ok())
            break;
        out.push_back(p.point);
        if (rectangle)
            break;
    }
    ok = out.size() >= 2;
}

// Applies one pick per intersection / crossing; later picks follow the pieces of earlier ones.
void batch(cmd::CommandContext& ctx, std::vector<std::pair<EntityId, Vec2>> picks, bool trimming,
           const EdgeSet& set) {
    std::map<EntityId, std::vector<EntityId>> pieces;
    std::size_t done = 0;
    // One undo step for the whole fence or crossing: committing after every pick would walk the
    // drawing (dimension and hatch associations) once per object.
    ctx.checkpoint();
    for (auto& [id, pt] : picks) {
        auto& list = pieces[id];
        if (list.empty())
            list.push_back(id);
        for (EntityId candidate : list) {
            const Entity* e = ctx.doc().find(candidate);
            if (!e || geom::distance(*e, pt, &ctx.doc()) > 1e-6)
                continue;
            if (auto ids = doOne(ctx, candidate, pt, trimming, set, false)) {
                list.insert(list.end(), ids->begin(), ids->end());
                ++done;
            }
            break;
        }
    }
    ctx.message(std::to_string(done) + (trimming ? " trimmed." : " extended."));
}

cmd::Task trimExtendLoop(cmd::CommandContext& ctx, bool trimMode) {
    ctx.commitOnCancel();
    EdgeSet set;
    while (!getFlag(ctx.host(), kQuickKey, false)) {
        cmd::SelectionOptions so;
        so.keywords = {"Mode"};
        so.filter = [&doc = ctx.doc()](const Entity& e) { return isUsableBoundary(doc, e); };
        auto sel =
            co_await ctx.getSelection(trimMode ? "Select cutting edges or [Mode] <select all>"
                                               : "Select boundary edges or [Mode] <select all>",
                                      so);
        if (sel.status == cmd::Status::Keyword) {
            cmd::KeywordOptions k;
            k.keywords = {"Quick", "Standard"};
            k.defaultKeyword = "Standard";
            auto r = co_await ctx.getKeyword("Mode [Quick/Standard]", k);
            if (!r.ok())
                co_return;
            putFlag(ctx.host(), kQuickKey, r.keyword == "Quick");
            continue;  // Quick: every object cuts; Standard: ask for the edges again
        }
        if (sel.status == cmd::Status::Ok) {
            set.ids = sel.ids;
            set.all = false;
        } else if (sel.status != cmd::Status::None) {
            co_return;
        }
        break;
    }
    for (;;) {
        cmd::EntityOptions eo;
        eo.keywords = {"Fence", "Crossing", "Edge", "Mode", "Undo"};
        if (trimMode)
            eo.keywords.insert(eo.keywords.begin() + 4, "Remove");
        eo.filter = [&doc = ctx.doc()](const Entity& e) { return isTrimTarget(doc, e); };
        auto pick =
            co_await ctx.getEntity(trimMode ? "Select object to trim or Shift-select to extend or "
                                              "[Fence/Crossing/Edge/Mode/Remove/Undo]"
                                            : "Select object to extend or Shift-select to trim or "
                                              "[Fence/Crossing/Edge/Mode/Undo]",
                                   eo);
        if (pick.status == cmd::Status::Keyword) {
            if (pick.keyword == "Undo") {
                ctx.message(ctx.rollbackToCheckpoint() ? "Last change undone."
                                                       : "Nothing to undo.");
            } else if (pick.keyword == "Remove") {
                cmd::SelectionOptions so;
                so.filter = [&doc = ctx.doc()](const Entity& e) { return isTrimTarget(doc, e); };
                so.allowPreselection = false;
                auto sel = co_await ctx.getSelection("Select objects to erase <exit>", so);
                if (sel.ok()) {
                    std::size_t n = 0;
                    for (EntityId id : sel.ids)
                        if (ctx.tx().erase(id))
                            ++n;
                    ctx.message(std::to_string(n) + " erased.");
                }
            } else if (pick.keyword == "Mode") {
                cmd::KeywordOptions k;
                k.keywords = {"Quick", "Standard"};
                k.defaultKeyword = getFlag(ctx.host(), kQuickKey, false) ? "Quick" : "Standard";
                auto r = co_await ctx.getKeyword("Mode [Quick/Standard]", k);
                if (r.ok()) {
                    const bool quick = r.keyword == "Quick";
                    putFlag(ctx.host(), kQuickKey, quick);
                    if (quick) {
                        set = EdgeSet{};  // every object that can cut
                    } else {
                        cmd::SelectionOptions so;
                        so.filter = [&doc = ctx.doc()](const Entity& e) {
                            return isUsableBoundary(doc, e);
                        };
                        auto sel = co_await ctx.getSelection(
                            trimMode ? "Select cutting edges <select all>"
                                     : "Select boundary edges <select all>",
                            so);
                        if (sel.status == cmd::Status::Ok) {
                            set.ids = sel.ids;
                            set.all = false;
                        }
                    }
                }
            } else if (pick.keyword == "Edge") {
                cmd::KeywordOptions k;
                k.keywords = {"Extend", "No extend"};
                k.defaultKeyword =
                    getFlag(ctx.host(), kEdgeExtendKey, false) ? "Extend" : "No extend";
                auto r = co_await ctx.getKeyword("Implied edge extension [Extend/No extend]", k);
                if (r.ok())
                    putFlag(ctx.host(), kEdgeExtendKey, r.keyword == "Extend");
            } else if (pick.keyword == "Fence") {
                std::vector<Vec2> fence;
                bool ok = false;
                co_await readPoints(ctx, "Specify first fence point",
                                    "Specify next fence point or <done>", false, fence, ok);
                if (!ok)
                    continue;
                std::vector<std::pair<EntityId, Vec2>> picks;
                for (EntityId id : ctx.doc().modelSpace()) {
                    const Entity* e = ctx.doc().find(id);
                    if (!e || !isEditable(ctx.doc(), *e))
                        continue;
                    for (std::size_t i = 0; i + 1 < fence.size(); ++i) {
                        Entity seg;
                        seg.data = LineData{fence[i], fence[i + 1]};
                        std::vector<geom::EntityIntersection> xs;
                        if (!geom::intersectEntities(seg, *e, xs))
                            continue;
                        for (const auto& x : xs)
                            picks.emplace_back(id, x.p);
                    }
                }
                batch(ctx, std::move(picks), trimMode, set);
            } else if (pick.keyword == "Crossing") {
                std::vector<Vec2> corners;
                bool ok = false;
                co_await readPoints(ctx, "Specify first corner", "Specify opposite corner", true,
                                    corners, ok);
                if (!ok)
                    continue;
                BBox2 window;
                window.expand(corners[0]);
                window.expand(corners[1]);
                std::vector<std::pair<EntityId, Vec2>> picks;
                for (EntityId id : ctx.doc().modelSpace()) {
                    const Entity* e = ctx.doc().find(id);
                    if (!e || !isEditable(ctx.doc(), *e) ||
                        !geom::crossesWindow(*e, window, &ctx.doc()))
                        continue;
                    if (auto p = geom::closestPoint(*e, window.center(), &ctx.doc()))
                        picks.emplace_back(id, *p);
                }
                batch(ctx, std::move(picks), trimMode, set);
            }
            continue;
        }
        if (!pick.ok())
            co_return;
        doOne(ctx, pick.id, pick.pickPoint, trimMode != pick.shift, set);
    }
}

class TrimCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "TRIM"; }
    std::vector<std::string> defaultAliases() const override { return {"TR"}; }
    std::string_view description() const override {
        return "Trim objects at cutting edges (Shift: extend)";
    }
    cmd::Task run(cmd::CommandContext& ctx) override { co_await trimExtendLoop(ctx, true); }
};

class ExtendCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "EXTEND"; }
    std::vector<std::string> defaultAliases() const override { return {"EX"}; }
    std::string_view description() const override {
        return "Extend objects to boundary edges (Shift: trim)";
    }
    cmd::Task run(cmd::CommandContext& ctx) override { co_await trimExtendLoop(ctx, false); }
};

}  // namespace

std::unique_ptr<cmd::Command> makeTrim() {
    return std::make_unique<TrimCommand>();
}
std::unique_ptr<cmd::Command> makeExtend() {
    return std::make_unique<ExtendCommand>();
}

}  // namespace tbcad::modifycmd
