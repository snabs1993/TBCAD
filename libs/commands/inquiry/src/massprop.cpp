// MASSPROP: area, perimeter, bounding box, centroid and second moments of a closed 2D shape.
#include <algorithm>
#include <cmath>

#include "extra.h"
#include "tbcad/geom/geom.h"

namespace tbcad::inquiry {

namespace {

std::vector<Vec2> outlineOfShape(const Entity& e) {
    std::vector<geom::CurveRef> curves;
    std::vector<Vec2> pts;
    if (!geom::curvesOf(e, curves))
        return pts;
    for (const auto& c : curves) {
        std::vector<Vec2> part;
        geom::flatten(c, 1e-4, part);
        for (const Vec2& p : part)
            if (pts.empty() || distance(pts.back(), p) > 1e-9)
                pts.push_back(p);
    }
    while (pts.size() > 1 && distance(pts.front(), pts.back()) <= 1e-9)
        pts.pop_back();
    return pts;
}

}  // namespace

std::optional<MassProps> massPropsOfEntity(const Entity& e) {
    const auto base = areaOfEntity(e);
    if (!base)
        return std::nullopt;
    if (const auto* pl = entityAs<PolylineData>(e))
        if (!pl->closed)
            return std::nullopt;
    std::vector<Vec2> pts = outlineOfShape(e);
    if (pts.size() < 3)
        return std::nullopt;

    // Shoelace sums about the origin, then the parallel-axis shift to the centroid.
    double a2 = 0, cx = 0, cy = 0, ixo = 0, iyo = 0, ixyo = 0;
    for (std::size_t i = 0; i < pts.size(); ++i) {
        const Vec2& p = pts[i];
        const Vec2& q = pts[(i + 1) % pts.size()];
        const double w = cross(p, q);
        a2 += w;
        cx += (p.x + q.x) * w;
        cy += (p.y + q.y) * w;
        ixo += (p.y * p.y + p.y * q.y + q.y * q.y) * w;
        iyo += (p.x * p.x + p.x * q.x + q.x * q.x) * w;
        ixyo += (p.x * q.y + 2 * p.x * p.y + 2 * q.x * q.y + q.x * p.y) * w;
    }
    if (std::abs(a2) < 1e-12)
        return std::nullopt;
    const double sign = a2 < 0 ? -1.0 : 1.0;  // normalise orientation
    const double area = std::abs(a2) / 2;
    MassProps m;
    m.area = base->area;
    m.perimeter = base->perimeter;
    m.centroid = {cx / (3 * a2), cy / (3 * a2)};
    const double ix = sign * ixo / 12, iy = sign * iyo / 12, ixy = sign * ixyo / 24;
    m.ixx = ix - area * m.centroid.y * m.centroid.y;
    m.iyy = iy - area * m.centroid.x * m.centroid.x;
    m.ixy = ixy - area * m.centroid.x * m.centroid.y;
    m.rx = std::sqrt(std::max(0.0, m.ixx / area));
    m.ry = std::sqrt(std::max(0.0, m.iyy / area));
    for (const Vec2& p : pts)
        m.bounds.expand(p);
    return m;
}

namespace {

class MassPropCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "MASSPROP"; }
    std::string_view description() const override {
        return "Area, centroid and moments of inertia of a closed shape";
    }

    cmd::Task run(cmd::CommandContext& ctx) override {
        cmd::EntityOptions eo;
        eo.filter = [](const Entity& e) { return massPropsOfEntity(e).has_value(); };
        auto picked = co_await ctx.getEntity("Select a closed object", eo);
        if (!picked.ok())
            co_return;
        const Entity* e = ctx.doc().find(picked.id);
        auto m = e ? massPropsOfEntity(*e) : std::nullopt;
        if (!m) {
            ctx.message("That object is not a closed shape.");
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
