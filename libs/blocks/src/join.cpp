#include <algorithm>
#include <cmath>
#include <map>

#include "internal.h"
#include "tbcad/blocks/display.h"

namespace tbcad::blocks::detail {

namespace {

thread_local int g_noJoin = 0;

struct Member {
    EntityId id{};
    BBox2 box{};
    std::vector<Vec2> ring{};
};

struct ClassCache {
    const Document* doc = nullptr;
    std::uint64_t revision = ~std::uint64_t{0};
    std::size_t entities = 0;
    double tolerance = 0;
    std::map<std::string, std::vector<Member>> classes{};
};
thread_local ClassCache g_cache;

double area(const std::vector<Vec2>& ring) {
    double a = 0;
    for (std::size_t i = 0; i < ring.size(); ++i)
        a += cross(ring[i], ring[(i + 1) % ring.size()]);
    return 0.5 * a;
}

// The body outline of an assembly: its largest closed polyline.
std::vector<Vec2> outlineOf(const DisplayList& list) {
    std::vector<Vec2> best;
    double bestArea = 0;
    for (const DisplayItem& item : list.items) {
        const auto* pl = std::get_if<DisplayPolyline>(&item);
        if (!pl || !pl->closed || pl->pts.size() < 3)
            continue;
        const double a = std::fabs(area(pl->pts));
        if (a > bestArea) {
            bestArea = a;
            best = pl->pts;
        }
    }
    return best;
}

double quantize(double tol) {
    if (!(tol > 0) || !std::isfinite(tol))
        return 1e-3;
    return std::exp2(std::floor(std::log2(tol)));
}

const std::vector<Member>& membersOf(const Document& doc, const std::string& cls,
                                     const geom::DisplayContext& ctx) {
    ClassCache& c = g_cache;
    const double tol = quantize(ctx.tolerance());
    if (c.doc != &doc || c.revision != doc.revision() || c.entities != doc.entityCount() ||
        c.tolerance != tol) {
        c.classes.clear();
        c.doc = &doc;
        c.revision = doc.revision();
        c.entities = doc.entityCount();
        c.tolerance = tol;
        NoJoinScope off;
        doc.forEachEntity([&](const Entity& e) {
            const auto* ins = entityAs<InsertData>(e);
            if (!ins || ins->joinOff || doc.ownerBlock(e.common.id))
                return;
            const BlockDef* def = doc.block(ins->block);
            if (!def || !def->assembly || def->assembly->joinClass.empty())
                return;
            DisplayList list;
            buildInsertDisplay(doc, e, ctx, list);
            Member m;
            m.id = e.common.id;
            m.ring = outlineOf(list);
            if (m.ring.size() < 3)
                return;
            for (Vec2 p : m.ring)
                m.box.expand(p);
            c.classes[def->assembly->joinClass].push_back(std::move(m));
        });
    }
    static const std::vector<Member> none;
    auto it = c.classes.find(cls);
    return it == c.classes.end() ? none : it->second;
}

enum class Where { Outside, Boundary, Inside };

Where classify(Vec2 p, const std::vector<Vec2>& ring, double eps) {
    bool inside = false;
    for (std::size_t i = 0; i < ring.size(); ++i) {
        const Vec2 a = ring[i];
        const Vec2 b = ring[(i + 1) % ring.size()];
        const Vec2 ab = b - a;
        const double len2 = dot(ab, ab);
        double t = len2 > 0 ? dot(p - a, ab) / len2 : 0;
        t = std::clamp(t, 0.0, 1.0);
        const Vec2 q{a.x + ab.x * t, a.y + ab.y * t};
        if (std::hypot(p.x - q.x, p.y - q.y) <= eps)
            return Where::Boundary;
        if ((a.y > p.y) != (b.y > p.y) && p.x < (b.x - a.x) * (p.y - a.y) / (b.y - a.y) + a.x)
            inside = !inside;
    }
    return inside ? Where::Inside : Where::Outside;
}

void addSplits(Vec2 a, Vec2 b, const std::vector<Vec2>& ring, std::vector<double>& ts) {
    const Vec2 r = b - a;
    const double rr = dot(r, r);
    if (rr <= 0)
        return;
    const double rl = std::sqrt(rr);
    for (std::size_t i = 0; i < ring.size(); ++i) {
        const Vec2 c = ring[i];
        const Vec2 d = ring[(i + 1) % ring.size()];
        const Vec2 s = d - c;
        const double sl = std::hypot(s.x, s.y);
        if (sl <= 0)
            continue;
        const double den = cross(r, s);
        if (std::fabs(den) > 1e-9 * rl * sl) {
            const double t = cross(c - a, s) / den;
            const double u = cross(c - a, r) / den;
            if (t > 0 && t < 1 && u >= 0 && u <= 1)
                ts.push_back(t);
        } else if (std::fabs(cross(c - a, r)) <= 1e-9 * rl * std::max(1.0, sl)) {
            for (Vec2 p : {c, d}) {
                const double t = dot(p - a, r) / rr;
                if (t > 0 && t < 1)
                    ts.push_back(t);
            }
        }
    }
}

BBox2 boxOf(const std::vector<Vec2>& pts) {
    BBox2 b;
    for (Vec2 p : pts)
        b.expand(p);
    return b;
}

bool overlaps(const BBox2& a, const BBox2& b, double eps) {
    return !(a.max.x < b.min.x - eps || b.max.x < a.min.x - eps || a.max.y < b.min.y - eps ||
             b.max.y < a.min.y - eps);
}

}  // namespace

NoJoinScope::NoJoinScope() {
    ++g_noJoin;
}
NoJoinScope::~NoJoinScope() {
    --g_noJoin;
}
bool NoJoinScope::active() {
    return g_noJoin > 0;
}

bool joinApplies(const Document& doc, const Entity& insert, const BlockDef& def,
                 const InsertData& data) {
    return !NoJoinScope::active() && def.assembly && !def.assembly->joinClass.empty() &&
           !data.joinOff && !doc.ownerBlock(insert.common.id);
}

void joinClip(const Document& doc, const Entity& insert, const BlockDef& def,
              const geom::DisplayContext& ctx, DisplayList& out, std::size_t first) {
    const std::vector<Member>& all = membersOf(doc, def.assembly->joinClass, ctx);
    const EntityId self = insert.common.id;
    std::vector<const Member*> others;
    for (const Member& m : all)
        if (m.id != self)
            others.push_back(&m);
    if (others.empty())
        return;
    const double eps = std::max(1e-6, ctx.tolerance() * 1e-3);

    std::vector<DisplayItem> kept(out.items.begin(), out.items.begin() + static_cast<long>(first));
    for (std::size_t n = first; n < out.items.size(); ++n) {
        const auto* pl = std::get_if<DisplayPolyline>(&out.items[n]);
        if (!pl || pl->pts.size() < 2) {
            kept.push_back(std::move(out.items[n]));
            continue;
        }
        const BBox2 box = boxOf(pl->pts);
        std::vector<const Member*> near;
        for (const Member* m : others)
            if (overlaps(box, m->box, eps))
                near.push_back(m);
        if (near.empty()) {
            kept.push_back(std::move(out.items[n]));
            continue;
        }
        auto dropped = [&](Vec2 p) {
            for (const Member* m : near) {
                if (p.x < m->box.min.x - eps || p.x > m->box.max.x + eps ||
                    p.y < m->box.min.y - eps || p.y > m->box.max.y + eps)
                    continue;
                const Where w = classify(p, m->ring, eps);
                // A shared edge is drawn by the member with the lower id only.
                if (w == Where::Inside || (w == Where::Boundary && m->id.value < self.value))
                    return true;
            }
            return false;
        };

        std::vector<std::vector<Vec2>> pieces;
        std::vector<Vec2> cur;
        bool anyDropped = false;
        const std::size_t segs = pl->closed ? pl->pts.size() : pl->pts.size() - 1;
        for (std::size_t i = 0; i < segs; ++i) {
            const Vec2 a = pl->pts[i];
            const Vec2 b = pl->pts[(i + 1) % pl->pts.size()];
            std::vector<double> ts;
            for (const Member* m : near)
                addSplits(a, b, m->ring, ts);
            ts.push_back(0);
            ts.push_back(1);
            std::sort(ts.begin(), ts.end());
            for (std::size_t k = 0; k + 1 < ts.size(); ++k) {
                if (ts[k + 1] - ts[k] < 1e-12)
                    continue;
                const Vec2 p{a.x + (b.x - a.x) * ts[k], a.y + (b.y - a.y) * ts[k]};
                const Vec2 q{a.x + (b.x - a.x) * ts[k + 1], a.y + (b.y - a.y) * ts[k + 1]};
                const Vec2 mid{(p.x + q.x) / 2, (p.y + q.y) / 2};
                if (dropped(mid)) {
                    anyDropped = true;
                    if (cur.size() >= 2)
                        pieces.push_back(std::move(cur));
                    cur.clear();
                } else {
                    if (cur.empty())
                        cur.push_back(p);
                    cur.push_back(q);
                }
            }
        }
        if (!anyDropped) {
            kept.push_back(std::move(out.items[n]));
            continue;
        }
        if (cur.size() >= 2)
            pieces.push_back(std::move(cur));
        for (auto& piece : pieces) {
            DisplayPolyline np;
            np.pts = std::move(piece);
            np.closed = false;
            np.style = pl->style;
            kept.emplace_back(std::move(np));
        }
    }
    out.items = std::move(kept);
}

}  // namespace tbcad::blocks::detail
