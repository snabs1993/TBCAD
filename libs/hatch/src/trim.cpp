#include <algorithm>
#include <cmath>

#include "tbcad/geom/entity.h"
#include "tbcad/geom/polybool.h"
#include "tbcad/hatch/boundary.h"
#include "tbcad/hatch/fill.h"
#include "tbcad/hatch/hatch.h"

namespace tbcad::hatch {

namespace {

// The ring as a closed polyline loop, with the arcs that were flattened brought back.
HatchLoop ringLoop(const geom::Ring& ring, double chord) {
    const geom::ArcFit fit = geom::fitArcs(ring, chord);
    HatchLoop loop;
    if (fit.circle) {
        HatchArcEdge a;
        a.center = fit.center;
        a.radius = fit.radius;
        a.startAngle = 0;
        a.endAngle = kTwoPi;
        loop.edges.emplace_back(a);
        return loop;
    }
    HatchPolylineEdge pl;
    for (const geom::ArcVertex& av : fit.vertices) {
        PolyVertex v;
        v.p = av.p;
        v.bulge = av.bulge;
        pl.v.push_back(v);
    }
    loop.edges.emplace_back(std::move(pl));
    return loop;
}

}  // namespace

Result<HatchData> trimHatch(const HatchData& hatch, Vec2 pick,
                            const std::vector<const Entity*>& edges) {
    const BBox2 box = hatchBounds(hatch);
    if (box.empty())
        return Error{ErrorCode::InvalidArgument, "The hatch has no area."};
    const double size = std::max({box.width(), box.height(), 1e-6});
    const double chord = std::clamp(size * 2e-5, 1e-6, 1e-2);

    geom::Region area;
    area.rings = loopRings(hatch, chord);
    area = geom::normalized(std::move(area));
    if (area.empty() || !geom::contains(area, pick))
        return Error{ErrorCode::NotFound, "Pick a point on the hatched area."};

    // The hatch's own outline and the cutting edges near it make the pieces; the piece under the
    // pick is taken away.
    std::vector<Entity> owned;
    owned.reserve(area.rings.size());
    for (const geom::Ring& r : area.rings) {
        PolylineData pl;
        pl.closed = true;
        for (Vec2 p : r)
            pl.v.push_back(PolyVertex{p});
        Entity e;
        e.data = std::move(pl);
        owned.push_back(std::move(e));
    }
    std::vector<Candidate> candidates;
    for (const Entity& e : owned)
        candidates.push_back({EntityId{}, &e});
    const BBox2 near{box.min - Vec2{size, size} * 1e-6, box.max + Vec2{size, size} * 1e-6};
    for (const Entity* e : edges) {
        if (!e || kind(*e) == EntityKind::Hatch)
            continue;
        const BBox2 b = geom::bounds(*e);
        if (!b.empty() && !b.intersects(near))
            continue;
        candidates.push_back({e->common.id, e});
    }
    BoundaryOptions opt;
    opt.flattenTolerance = chord;
    auto piece = findBoundary(candidates, pick, opt);
    if (!piece)
        return piece.error();

    HatchData one;
    one.loops = piece.value().loops;
    geom::Region cut;
    cut.rings = loopRings(one, chord);
    geom::Region rest = geom::boolean(area, geom::normalized(std::move(cut)),
                                      geom::BoolOp::Subtract, std::max(1e-7, size * 1e-9));
    // Nothing left, or nothing taken: no edge cuts the hatch at the pick.
    const double before = geom::area(area);
    const double after = rest.empty() ? 0 : geom::area(rest);
    if (after <= before * 1e-9 || after >= before * (1 - 1e-7))
        return Error{ErrorCode::NotFound, "No cutting edge crosses the hatch here."};

    HatchData out = hatch;
    out.loops.clear();
    for (const geom::Ring& r : rest.rings) {
        HatchLoop loop = ringLoop(r, chord * 4);
        // normalized(): outer boundaries counter-clockwise, holes clockwise.
        double twice = 0;
        for (std::size_t i = 0; i < r.size(); ++i)
            twice += cross(r[i], r[(i + 1) % r.size()]);
        loop.outer = twice > 0;
        out.loops.push_back(std::move(loop));
    }
    out.sourceBoundary.clear();  // a trimmed hatch no longer follows its boundary
    return out;
}

}  // namespace tbcad::hatch
