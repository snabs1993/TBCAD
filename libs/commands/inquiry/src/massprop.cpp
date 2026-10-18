// MASSPROP: area, perimeter, bounding box, centroid and second moments of a closed 2D shape.
#include <algorithm>
#include <cmath>
#include <cstdio>

#include "extra.h"
#include "tbcad/geom/geom.h"
#include "tbcad/geom/polybool.h"

namespace tbcad::inquiry {

namespace {

// The outline of a closed shape as a polygon. The deviation from the true curve is at most
// `1e-9` of the shape's size (but never more than 1e-4 mm), so the sums below are good to a few
// parts in a hundred million.
std::vector<Vec2> outlineOfShape(const Entity& e) {
    std::vector<geom::CurveRef> curves;
    std::vector<Vec2> pts;
    if (!geom::curvesOf(e, curves))
        return pts;
    BBox2 box = curves.front().bounds();
    for (const auto& c : curves) {
        box.expand(c.bounds().min);
        box.expand(c.bounds().max);
    }
    const double tol = std::clamp(distance(box.min, box.max) * 1e-9, 1e-10, 1e-4);
    geom::outlinePoints(e, tol, pts);
    return pts;
}

bool closedShape(const Entity& e) {
    if (const auto* pl = entityAs<PolylineData>(e))
        return pl->closed;
    return areaOfEntity(e).has_value();
}

}  // namespace

std::optional<MassProps> massPropsOfEntities(const std::vector<const Entity*>& entities) {
    geom::Region region;
    double perimeter = 0;
    for (const Entity* e : entities) {
        if (!e || !closedShape(*e))
            return std::nullopt;
        std::vector<Vec2> pts = outlineOfShape(*e);
        if (pts.size() < 3)
            return std::nullopt;
        for (std::size_t i = 0; i < pts.size(); ++i)
            perimeter += distance(pts[i], pts[(i + 1) % pts.size()]);
        region.rings.push_back(std::move(pts));
    }
    if (region.rings.empty())
        return std::nullopt;
    // A ring inside a ring is a hole: outer rings come out counter-clockwise, holes clockwise, so
    // the signed sums below subtract the holes.
    region = geom::normalized(std::move(region), 1e-12);
    if (region.rings.empty())
        return std::nullopt;

    // Sums about the first vertex (survey coordinates would otherwise lose every digit).
    const Vec2 ref = region.rings.front().front();
    double a2 = 0, sx = 0, sy = 0, sxx = 0, syy = 0, sxy = 0;
    BBox2 bounds;
    for (const auto& ring : region.rings) {
        for (std::size_t i = 0; i < ring.size(); ++i) {
            const Vec2 p = ring[i] - ref, q = ring[(i + 1) % ring.size()] - ref;
            const double w = cross(p, q);
            a2 += w;
            sx += (p.x + q.x) * w;
            sy += (p.y + q.y) * w;
            sxx += (p.x * p.x + p.x * q.x + q.x * q.x) * w;
            syy += (p.y * p.y + p.y * q.y + q.y * q.y) * w;
            sxy += (p.x * q.y + 2 * p.x * p.y + 2 * q.x * q.y + q.x * p.y) * w;
            bounds.expand(ring[i]);
        }
    }
    if (std::abs(a2) < 1e-18)
        return std::nullopt;
    const double area = a2 / 2;
    const Vec2 c{sx / (3 * a2), sy / (3 * a2)};  // relative to ref
    MassProps m;
    m.area = area;
    m.perimeter = perimeter;
    m.centroid = ref + c;
    // About the origin at ref: Ix = sum(y^2) etc.; shift to the centroid with the parallel axis
    // rule.
    const double ixx0 = sxx / 12, iyy0 = syy / 12, ixy0 = sxy / 24;
    // Naming follows the old code: ixx is the moment about the x axis (integral of y^2).
    m.ixx = iyy0 - area * c.y * c.y;
    m.iyy = ixx0 - area * c.x * c.x;
    m.ixy = ixy0 - area * c.x * c.y;
    m.rx = std::sqrt(std::max(0.0, m.ixx / area));
    m.ry = std::sqrt(std::max(0.0, m.iyy / area));
    m.bounds = bounds;
    return m;
}

std::optional<MassProps> massPropsOfEntity(const Entity& e) {
    const auto base = areaOfEntity(e);
    if (!base)
        return std::nullopt;
    if (const auto* pl = entityAs<PolylineData>(e))
        if (!pl->closed)
            return std::nullopt;
    auto m = massPropsOfEntities({&e});
    if (!m)
        return std::nullopt;
    m->area = base->area;
    m->perimeter = base->perimeter;
    return m;
}

namespace {

class MassPropCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "MASSPROP"; }
    std::string_view description() const override {
        return "Area, centroid and moments of inertia of closed shapes (rings inside rings are "
               "holes)";
    }

    cmd::Task run(cmd::CommandContext& ctx) override {
        cmd::SelectionOptions so;
        so.filter = [](const Entity& e) { return closedShape(e) && massPropsOfEntity(e); };
        auto picked = co_await ctx.getSelection("Select closed objects", so);
        if (!picked.ok() || picked.ids.empty())
            co_return;
        std::vector<const Entity*> es;
        for (EntityId id : picked.ids)
            if (const Entity* e = ctx.doc().find(id))
                es.push_back(e);
        // One object: its exact area. Several: one region, a ring inside a ring is a hole.
        auto m = es.size() == 1 ? massPropsOfEntity(*es[0]) : massPropsOfEntities(es);
        if (!m) {
            ctx.message("Those objects are not closed shapes.");
            co_return;
        }
        const Precision p = 4;
        const Document& d = ctx.doc();
        const double k = mmPerUnit(d.header().units);
        auto fix = [&](double v, double scale) {
            char buf[64];
            std::snprintf(buf, sizeof buf, "%.*f", p, v / scale);
            return std::string(buf);
        };
        const std::string u(unitSuffix(d.header().units));
        ctx.message("Area:        " + formatAreaIn(d, m->area, p));
        ctx.message("Perimeter:   " + formatLengthIn(d, m->perimeter, p));
        ctx.message("Bounding box: X " + formatLengthIn(d, m->bounds.min.x, p) + " .. " +
                    formatLengthIn(d, m->bounds.max.x, p) + "   Y " +
                    formatLengthIn(d, m->bounds.min.y, p) + " .. " +
                    formatLengthIn(d, m->bounds.max.y, p));
        ctx.message("Centroid:    X " + formatLengthIn(d, m->centroid.x, p) + "   Y " +
                    formatLengthIn(d, m->centroid.y, p));
        ctx.message("Moments of inertia about the centroid:  Ixx " + fix(m->ixx, k * k * k * k) +
                    " " + u + "^4   Iyy " + fix(m->iyy, k * k * k * k) + " " + u + "^4");
        ctx.message("Product of inertia:  Ixy " + fix(m->ixy, k * k * k * k) + " " + u + "^4");
        ctx.message("Radii of gyration:  X " + formatLengthIn(d, m->rx, p) + "   Y " +
                    formatLengthIn(d, m->ry, p));
    }
};

}  // namespace

std::unique_ptr<cmd::Command> makeMassProp() {
    return std::make_unique<MassPropCommand>();
}

}  // namespace tbcad::inquiry
