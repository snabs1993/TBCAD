#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <memory>
#include <string>

#include "commands.h"
#include "common.h"

namespace tbcad::draw2::detail {

namespace {

constexpr const char* kArcLength = "revcloud.arclength";
constexpr const char* kCalligraphy = "revcloud.calligraphy";
// REVCLOUDCREATEMODE: how the first point is used. 0 freehand, 1 rectangular, 2 polygonal.
constexpr const char* kCreateMode = "revcloud.createmode";
enum CreateMode { kFreehand = 0, kRectangular = 1, kPolygonal = 2 };
constexpr double kDefaultArcLength = 10;
// Each cloud arc sweeps 120 degrees: bulge = tan(sweep / 4).
const double kCloudBulge = std::tan(kPi / 6);

// More arcs than this would make an object too heavy to draw: the command asks for a longer arc.
constexpr std::size_t kMaxArcs = geom::kMaxCloudArcs;

// `ring` without repeated points (a polyline vertex given twice, the last point on the first).
std::vector<Vec2> distinct(const std::vector<Vec2>& ring) {
    std::vector<Vec2> out;
    for (const Vec2& p : ring)
        if (out.empty() || !tol::isZeroLength(p - out.back()))
            out.push_back(p);
    while (out.size() > 1 && tol::isZeroLength(out.front() - out.back()))
        out.pop_back();
    return out;
}

// How many arcs cloud() makes along `ring`.
double arcCount(const std::vector<Vec2>& ring, bool corners, double arcLength) {
    return geom::cloudArcCount(ring, true, corners, arcLength);
}

std::string shortNumber(double v) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.4g", v);
    return buf;
}

// The message when `ring` needs too many arcs, or empty.
std::string tooManyArcs(const std::vector<Vec2>& ring, bool corners, double arcLength) {
    const double n = arcCount(ring, corners, arcLength);
    if (std::isfinite(n) && n <= static_cast<double>(kMaxArcs))
        return {};
    double total = 0;
    for (std::size_t i = 0; i < ring.size(); ++i)
        total += distance(ring[i], ring[(i + 1) % ring.size()]);
    return "The arc length is too small for a cloud this size; use " +
           shortNumber(total / static_cast<double>(kMaxArcs) * 1.01) + " or more.";
}

// A closed polyline of arcs along `ring`. With `corners` the ring's own vertices stay cloud
// vertices and every edge is divided into pieces no longer than `arcLength`; otherwise the ring is
// divided into equal pieces by length. The arcs bulge outwards (inwards with `reverse`).
PolylineData cloud(const std::vector<Vec2>& ring, bool corners, double arcLength, bool reverse,
                   bool calligraphy = false) {
    return geom::cloudClosed(ring, corners, arcLength, reverse, calligraphy);
}

// An open cloud along `path`: each piece no longer than `arcLength`, the arcs on the side away from
// the area the path encloses (the other side with `reverse`).
PolylineData openCloud(const std::vector<Vec2>& path, double arcLength, bool reverse,
                       bool calligraphy) {
    return geom::cloudOpen(path, arcLength, reverse, calligraphy);
}

// Adds the cloud with the current properties, marked so that it can be laid again later.
void addCloud(cmd::CommandContext& ctx, PolylineData d, double arcLength, bool corners = true) {
    Entity e{{}, std::move(d)};
    applyCurrentProperties(ctx.doc().header(), e);
    geom::markCloud(e, arcLength, corners);
    if (auto id = ctx.tx().add(std::move(e)); !id)
        ctx.message(id.error().message);
}

// A place on a closed polyline: segment k (from vertex k to k + 1) at u in [0, 1] along it.
struct PolyPos {
    int k = 0;
    double u = 0;
    double value() const { return k + u; }
};

Vec2 pointAt(const PolylineData& pl, PolyPos at) {
    const std::size_t n = pl.v.size();
    const Vec2 a = pl.v[at.k % n].p, b = pl.v[(at.k + 1) % n].p;
    const double bulge = pl.v[at.k % n].bulge;
    const auto arc = geom::bulgeArc(a, b, bulge);
    if (!arc)
        return lerp(a, b, at.u);
    const double a0 = std::atan2(a.y - arc->center.y, a.x - arc->center.x);
    const double t = a0 + 4 * std::atan(bulge) * at.u;
    return arc->center + Vec2{std::cos(t), std::sin(t)} * arc->radius;
}

// The closest place on the closed polyline to p.
PolyPos closestPos(const PolylineData& pl, Vec2 p) {
    const std::size_t n = pl.v.size();
    PolyPos best;
    double bestD = std::numeric_limits<double>::infinity();
    for (std::size_t k = 0; k < n; ++k) {
        const Vec2 a = pl.v[k].p, b = pl.v[(k + 1) % n].p;
        double u = 0;
        if (const auto arc = geom::bulgeArc(a, b, pl.v[k].bulge)) {
            const double sweep = 4 * std::atan(pl.v[k].bulge);
            const double a0 = std::atan2(a.y - arc->center.y, a.x - arc->center.x);
            double rel = std::atan2(p.y - arc->center.y, p.x - arc->center.x) - a0;
            if (sweep < 0)
                rel = -rel;
            rel = std::fmod(rel + 4 * kPi, kTwoPi);  // [0, 2 pi) in the direction of travel
            const double s = std::abs(sweep);
            u = rel <= s ? rel / s : (rel - s < kTwoPi - rel ? 1.0 : 0.0);
        } else {
            const Vec2 d = b - a;
            const double l2 = d.x * d.x + d.y * d.y;
            u = l2 > 0 ? std::clamp(((p.x - a.x) * d.x + (p.y - a.y) * d.y) / l2, 0.0, 1.0) : 0;
        }
        const PolyPos at{static_cast<int>(k), u};
        const double dd = distance(pointAt(pl, at), p);
        if (dd < bestD) {
            bestD = dd;
            best = at;
        }
    }
    return best;
}

// The piece of the closed polyline from `from` forward to `to` (going round past the seam when
// `to` comes first), as vertices with bulges; the last vertex is `to`.
std::vector<PolyVertex> piece(const PolylineData& pl, PolyPos from, PolyPos to) {
    const int n = static_cast<int>(pl.v.size());
    std::vector<PolyVertex> out;
    auto partBulge = [&](int k, double u0, double u1) {
        const double b = pl.v[k % n].bulge;
        return b == 0 ? 0.0 : std::tan(std::atan(b) * (u1 - u0));
    };
    double end = to.value();
    if (end <= from.value() + 1e-12)
        end += n;
    PolyPos at = from;
    for (;;) {
        PolyVertex v;
        v.p = pointAt(pl, at);
        const double segEnd = at.k + 1;
        if (end <= segEnd + 1e-12) {
            v.bulge = partBulge(at.k, at.u, end - at.k);
            out.push_back(v);
            break;
        }
        v.bulge = partBulge(at.k, at.u, 1);
        out.push_back(v);
        at = {at.k + 1, 0};
    }
    PolyVertex last;
    last.p = pointAt(pl, to);
    out.push_back(last);
    // drop zero-length pieces (from or to on a vertex)
    std::vector<PolyVertex> clean;
    for (const PolyVertex& v : out) {
        if (!clean.empty() && tol::isZeroLength(v.p - clean.back().p)) {
            clean.back().bulge = v.bulge;
            continue;
        }
        clean.push_back(v);
    }
    return clean;
}

// Shortest distance from p to a run of vertices with bulges (the last vertex ends it).
double distanceToRun(const std::vector<PolyVertex>& run, Vec2 p) {
    if (run.size() < 2)
        return run.empty() ? std::numeric_limits<double>::infinity() : distance(run[0].p, p);
    double best = std::numeric_limits<double>::infinity();
    for (int k = 0; k + 1 < static_cast<int>(run.size()); ++k) {
        PolylineData seg;
        seg.v = {run[k], run[k + 1]};
        seg.v[1].bulge = 0;
        best = std::min(best, distance(pointAt(seg, closestPos(seg, p)), p));
    }
    return best;
}

struct Ring {
    std::vector<Vec2> pts{};
    bool corners = false;
};

std::optional<Ring> ringOf(const Entity& e) {
    Ring r;
    if (const auto* pl = std::get_if<PolylineData>(&e.data)) {
        if (!pl->closed || pl->v.size() < 3)
            return std::nullopt;
        const bool straight = std::all_of(pl->v.begin(), pl->v.end(),
                                          [](const PolyVertex& v) { return v.bulge == 0; });
        if (straight) {
            for (const auto& v : pl->v)
                r.pts.push_back(v.p);
            r.corners = true;
            return r;
        }
    } else if (kind(e) == EntityKind::Circle || kind(e) == EntityKind::Ellipse ||
               kind(e) == EntityKind::Spline) {
        // handled below
    } else {
        return std::nullopt;
    }
    std::vector<geom::CurveRef> curves;
    if (!geom::curvesOf(e, curves) || curves.empty())
        return std::nullopt;
    for (const auto& c : curves) {
        std::vector<Vec2> pts;
        geom::flatten(c, 0.05, pts);
        if (!pts.empty())
            pts.pop_back();
        r.pts.insert(r.pts.end(), pts.begin(), pts.end());
    }
    if (r.pts.size() < 3)
        return std::nullopt;
    return r;
}

bool cloudable(const Entity& e) {
    switch (kind(e)) {
        case EntityKind::Circle:
        case EntityKind::Ellipse:
            return true;
        case EntityKind::Polyline:
        case EntityKind::Spline:
            return ringOf(e).has_value();
        default:
            return false;
    }
}

// REVCLOUD [REVC]: a revision cloud from picked points (Enter closes it) or from a closed object.
// Arclength sets the longest arc chord; Esc draws nothing.
class RevCloudCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "REVCLOUD"; }
    std::vector<std::string> defaultAliases() const override { return {"REVC"}; }
    std::string_view description() const override { return "Draw a revision cloud"; }

    cmd::Task run(cmd::CommandContext& ctx) override {
        for (;;) {
            const double len = settingNumber(ctx, kArcLength, kDefaultArcLength);
            const int mode = static_cast<int>(settingNumber(ctx, kCreateMode, kPolygonal));
            cmd::PointOptions o;
            o.keywords = {"Arclength", "Object", "Rectangular", "Polygonal",
                          "Freehand",  "Style",  "Modify"};
            o.allowNone = true;
            const char* what = mode == kRectangular ? "first corner point" : "start point";
            auto first = co_await ctx.getPoint(
                "Specify " + std::string(what) + " (arc length " + shortNumber(len) + ", " +
                    (mode == kRectangular ? "rectangular"
                     : mode == kFreehand  ? "freehand"
                                          : "polygonal") +
                    ") or [Arclength/Object/Rectangular/Polygonal/Freehand/Style/Modify] <Object>",
                std::move(o));
            if (first.status == cmd::Status::None) {  // Enter: like AutoCAD, pick an object
                co_await fromObject(ctx, len);
                co_return;
            }
            if (first.status == cmd::Status::Keyword) {
                const std::string& kw = first.keyword;
                if (kw == "Arclength") {
                    cmd::DistanceOptions lo;
                    lo.defaultValue = len;
                    lo.allowZero = false;
                    auto l =
                        co_await ctx.getDistance("Specify approximate arc length", std::move(lo));
                    if (!l.ok())
                        co_return;
                    saveSetting(ctx, kArcLength, l.value);
                    continue;
                }
                if (kw == "Style") {
                    cmd::KeywordOptions so;
                    so.keywords = {"Normal", "Calligraphy"};
                    so.defaultKeyword =
                        settingNumber(ctx, kCalligraphy, 0) > 0 ? "Calligraphy" : "Normal";
                    auto st =
                        co_await ctx.getKeyword("Arc style [Normal/Calligraphy]", std::move(so));
                    if (!st.ok())
                        co_return;
                    saveSetting(ctx, kCalligraphy, st.keyword == "Calligraphy" ? 1 : 0);
                    continue;
                }
                if (kw == "Modify") {
                    co_await modify(ctx, len);
                    co_return;
                }
                if (kw == "Rectangular" || kw == "Polygonal" || kw == "Freehand") {
                    // as REVCLOUDCREATEMODE: remembered for the next cloud
                    saveSetting(ctx, kCreateMode,
                                kw == "Rectangular" ? kRectangular
                                : kw == "Freehand"  ? kFreehand
                                                    : kPolygonal);
                    continue;
                }
                co_await fromObject(ctx, len);
                co_return;
            }
            if (!first.ok())
                co_return;
            if (mode == kRectangular)
                co_await fromRectangle(ctx, first.point, len);
            else if (mode == kFreehand)
                co_await freehand(ctx, first.point, len);
            else
                co_await fromPoints(ctx, first.point, len);
            co_return;
        }
    }

private:
    static cmd::Task fromPoints(cmd::CommandContext& ctx, Vec2 start, double len) {
        auto* c = &ctx;
        std::vector<Vec2> pts{start};
        for (;;) {
            cmd::PointOptions o;
            o.base = pts.back();
            if (pts.size() > 1)
                o.keywords = {"Undo"};
            o.preview = [c, pts, len](Vec2 cursor, DisplayList& out) {
                std::vector<Vec2> p = pts;
                p.push_back(cursor);
                if (p.size() >= 3)
                    previewEntity(*c, makeEntity(cloud(p, true, len, false)), out);
            };
            auto r = co_await ctx.getPoint(
                promptWith("Specify next point or Enter to close", o.keywords), std::move(o));
            if (r.status == cmd::Status::None)
                break;
            if (r.status == cmd::Status::Keyword) {
                pts.pop_back();
                continue;
            }
            if (!r.ok())
                co_return;
            if (!tol::isZeroLength(r.point - pts.back()))
                pts.push_back(r.point);
        }
        if (distinct(pts).size() < 3 || std::abs(ringArea(pts)) < tol::kLinear) {
            ctx.message("A revision cloud needs at least three points that enclose an area.");
            co_return;
        }
        if (const std::string m = tooManyArcs(pts, true, len); !m.empty()) {
            ctx.message(m);
            co_return;
        }
        addCloud(ctx, cloud(pts, true, len, false, settingNumber(ctx, kCalligraphy, 0) > 0), len);
    }

    static cmd::Task fromRectangle(cmd::CommandContext& ctx, Vec2 corner, double len) {
        auto* c = &ctx;
        cmd::PointOptions po;
        po.base = corner;
        po.preview = [c, corner, len](Vec2 cursor, DisplayList& out) {
            if (std::abs(corner.x - cursor.x) < tol::kLinear ||
                std::abs(corner.y - cursor.y) < tol::kLinear)
                return;
            const std::vector<Vec2> ring{
                corner, {cursor.x, corner.y}, cursor, {corner.x, cursor.y}};
            if (tooManyArcs(ring, true, len).empty())
                previewEntity(*c, makeEntity(cloud(ring, true, len, false)), out);
        };
        auto b = co_await ctx.getPoint("Specify opposite corner point", std::move(po));
        if (!b.ok())
            co_return;
        if (std::abs(corner.x - b.point.x) < tol::kLinear ||
            std::abs(corner.y - b.point.y) < tol::kLinear) {
            ctx.message("The two corners must differ in both directions.");
            co_return;
        }
        const std::vector<Vec2> ring{corner, {b.point.x, corner.y}, b.point, {corner.x, b.point.y}};
        if (const std::string m = tooManyArcs(ring, true, len); !m.empty()) {
            ctx.message(m);
            co_return;
        }
        addCloud(ctx, cloud(ring, true, len, false, settingNumber(ctx, kCalligraphy, 0) > 0), len);
    }

    // Freehand: moving the cursor lays an arc each time it gets an arc length away from the last
    // one; picked points are added too. Coming back near the start closes the cloud ("Revision
    // cloud finished"); Enter ends an open cloud and asks which way its arcs face.
    static cmd::Task freehand(cmd::CommandContext& ctx, Vec2 start, double len) {
        auto* c = &ctx;
        auto path = std::make_shared<std::vector<Vec2>>(std::vector<Vec2>{start});
        bool closed = false;
        auto nearStart = [path, len](Vec2 p) {
            return path->size() >= 3 && distance(p, path->front()) <= len * 0.5 + tol::kLinear;
        };
        for (;;) {
            cmd::PointOptions o;
            o.base = path->back();
            o.allowNone = true;
            o.preview = [c, path, len](Vec2 cursor, DisplayList& out) {
                // follow the cursor: a vertex every arc length along its way
                while (distance(cursor, path->back()) >= len && path->size() < kMaxArcs) {
                    const Vec2 d = cursor - path->back();
                    path->push_back(path->back() + d * (len / std::hypot(d.x, d.y)));
                }
                std::vector<Vec2> p = *path;
                p.push_back(cursor);
                if (p.size() >= 2)
                    previewEntity(*c, makeEntity(openCloud(p, len, false, false)), out);
            };
            auto r = co_await ctx.getPoint("Guide crosshairs along cloud path (Enter ends it)",
                                           std::move(o));
            if (r.status == cmd::Status::None)
                break;
            if (!r.ok())
                co_return;
            if (nearStart(r.point)) {
                closed = true;
                break;
            }
            if (!tol::isZeroLength(r.point - path->back()))
                path->push_back(r.point);
            if (path->size() >= kMaxArcs)
                break;
        }
        const bool calligraphy = settingNumber(ctx, kCalligraphy, 0) > 0;
        if (closed) {
            if (distinct(*path).size() < 3 || std::abs(ringArea(*path)) < tol::kLinear) {
                ctx.message("A revision cloud needs at least three points that enclose an area.");
                co_return;
            }
            if (const std::string m = tooManyArcs(*path, true, len); !m.empty()) {
                ctx.message(m);
                co_return;
            }
            ctx.message("Revision cloud finished.");
            addCloud(ctx, cloud(*path, true, len, false, calligraphy), len);
            co_return;
        }
        if (distinct(*path).size() < 2) {
            ctx.message("Move the cursor along the cloud path first.");
            co_return;
        }
        if (const std::string m = tooManyArcs(*path, true, len); !m.empty()) {
            ctx.message(m);
            co_return;
        }
        cmd::KeywordOptions ko;
        ko.keywords = {"Yes", "No"};
        ko.defaultKeyword = "No";
        auto rev = co_await ctx.getKeyword("Reverse direction [Yes/No] <No>", std::move(ko));
        if (!rev.ok())
            co_return;
        addCloud(ctx, openCloud(*path, len, rev.keyword == "Yes", calligraphy), len);
    }

    // Modify (as AutoCAD): pick a polyline where the new part starts, give points along the new
    // outline until one lands on the polyline again (or Enter: the nearest place to the last
    // point), then pick the side of the old outline to erase. The new part is a cloud with arcs
    // facing out of the result; Reverse turns them in.
    static cmd::Task modify(cmd::CommandContext& ctx, double len) {
        auto* c = &ctx;
        cmd::EntityOptions eo;
        eo.filter = [c](const Entity& x) {
            const auto* pl = entityAs<PolylineData>(x);
            return pl && pl->closed && pl->v.size() >= 2 && !c->doc().effective(x.common).locked;
        };
        auto picked = co_await ctx.getEntity("Select polyline to modify", std::move(eo));
        if (!picked.ok())
            co_return;
        const Entity* ent = ctx.doc().find(picked.id);
        if (!ent)
            co_return;
        const PolylineData old = *entityAs<PolylineData>(*ent);
        const PolyPos from = closestPos(old, picked.pickPoint);
        const Vec2 start = pointAt(old, from);
        const double onTol = std::max(len * 0.1, 1e-6);

        std::vector<Vec2> pts{start};
        std::optional<PolyPos> to;
        for (;;) {
            cmd::PointOptions o;
            o.base = pts.back();
            if (pts.size() > 1)
                o.keywords = {"Undo"};
            o.allowNone = pts.size() > 1;
            o.preview = [c, pts, len](Vec2 cursor, DisplayList& out) {
                std::vector<Vec2> p = pts;
                p.push_back(cursor);
                previewEntity(*c, makeEntity(openCloud(p, len, false, false)), out);
            };
            auto r =
                co_await ctx.getPoint(promptWith("Specify next point", o.keywords), std::move(o));
            if (r.status == cmd::Status::Keyword) {
                pts.pop_back();
                continue;
            }
            if (r.status == cmd::Status::None) {
                to = closestPos(old, pts.back());
                if (distance(pointAt(old, *to), pts.back()) <= onTol)
                    pts.pop_back();  // the last point was on the polyline already
                break;
            }
            if (!r.ok())
                co_return;
            const PolyPos at = closestPos(old, r.point);
            if (distance(pointAt(old, at), r.point) <= onTol &&
                !tol::isZeroLength(pointAt(old, at) - start)) {
                to = at;
                break;
            }
            if (!tol::isZeroLength(r.point - pts.back()))
                pts.push_back(r.point);
        }
        const Vec2 end = pointAt(old, *to);
        if (tol::isZeroLength(end - start)) {
            ctx.message("The new part must end somewhere else on the polyline.");
            co_return;
        }
        pts.push_back(end);

        const std::vector<PolyVertex> forward = piece(old, from, *to);   // start .. end
        const std::vector<PolyVertex> backward = piece(old, *to, from);  // end .. start
        auto side = co_await ctx.getPoint("Pick a side to erase");
        if (!side.ok())
            co_return;
        const bool eraseForward =
            distanceToRun(forward, side.point) <= distanceToRun(backward, side.point);
        cmd::KeywordOptions ko;
        ko.keywords = {"Yes", "No"};
        ko.defaultKeyword = "No";
        auto rev = co_await ctx.getKeyword("Reverse direction [Yes/No] <No>", std::move(ko));
        if (!rev.ok())
            co_return;

        // The kept part runs from one end of the new part to the other; the new part closes it.
        std::vector<PolyVertex> kept =
            eraseForward ? backward : forward;  // end..start / start..end
        std::vector<Vec2> fresh = pts;          // start .. end
        if (!eraseForward)
            std::reverse(fresh.begin(), fresh.end());  // end .. start
        kept.pop_back();  // its last vertex is where the new part starts
        PolylineData result;
        result.closed = true;
        result.v = kept;
        std::vector<Vec2> ringPts;
        for (const PolyVertex& v : kept)
            ringPts.push_back(v.p);
        for (std::size_t i = 0; i + 1 < fresh.size(); ++i)
            ringPts.push_back(fresh[i]);
        if (distinct(ringPts).size() < 3 || std::abs(ringArea(ringPts)) < tol::kLinear) {
            ctx.message("The modified cloud would enclose no area.");
            co_return;
        }
        // the new part, in the result's direction, as cloud arcs facing out of the result
        PolylineData part = openCloud(fresh, len, false, settingNumber(ctx, kCalligraphy, 0) > 0);
        double sign = ringArea(ringPts) >= 0 ? 1.0 : -1.0;
        if (rev.keyword == "Yes")
            sign = -sign;
        for (std::size_t i = 0; i + 1 < part.v.size(); ++i) {
            PolyVertex v = part.v[i];
            v.bulge = sign * kCloudBulge;
            result.v.push_back(v);
        }
        if (const std::string m = tooManyArcs(ringPts, true, len); !m.empty()) {
            ctx.message(m);
            co_return;
        }
        if (!ctx.tx().modify(picked.id, [&](Entity& x) {
                x.data = result;
                geom::markCloud(x, len, true);
            })) {
            ctx.message("The polyline could not be changed.");
            co_return;
        }
        ctx.setUndoLabel("REVCLOUD Modify");
    }

    static cmd::Task fromObject(cmd::CommandContext& ctx, double len) {
        cmd::EntityOptions eo;
        auto* c = &ctx;
        eo.filter = [c](const Entity& x) {
            return cloudable(x) && !c->doc().effective(x.common).locked;
        };
        auto e = co_await ctx.getEntity("Select a closed object", std::move(eo));
        if (!e.ok())
            co_return;
        const Entity* ent = ctx.doc().find(e.id);
        auto ring = ent ? ringOf(*ent) : std::nullopt;
        if (!ring) {
            ctx.message("That object cannot be turned into a revision cloud.");
            co_return;
        }
        if (const std::string m = tooManyArcs(ring->pts, ring->corners, len); !m.empty()) {
            ctx.message(m);
            co_return;
        }
        cmd::KeywordOptions ko;
        ko.keywords = {"Yes", "No"};
        ko.defaultKeyword = "No";
        auto rev = co_await ctx.getKeyword("Reverse direction [Yes/No] <No>", std::move(ko));
        if (!rev.ok())
            co_return;
        ent = ctx.doc().find(e.id);
        if (!ent)
            co_return;
        // Like AutoCAD the object is converted: the cloud takes its place and its properties
        // (layer, colour, linetype, ...) and the object is erased.
        Entity cloudEntity{ent->common, cloud(ring->pts, ring->corners, len, rev.keyword == "Yes",
                                              settingNumber(ctx, kCalligraphy, 0) > 0)};
        cloudEntity.common.id = {};
        geom::markCloud(cloudEntity, len, ring->corners);
        auto id = ctx.tx().add(std::move(cloudEntity));
        if (!id) {
            ctx.message(id.error().message);
            co_return;
        }
        (void)ctx.tx().erase(e.id);
    }
};

}  // namespace

std::unique_ptr<cmd::Command> makeRevCloud() {
    return std::make_unique<RevCloudCommand>();
}

}  // namespace tbcad::draw2::detail
