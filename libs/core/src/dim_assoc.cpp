#include "tbcad/core/dim_assoc.h"

#include <cmath>

#include "tbcad/core/document.h"

namespace tbcad {

namespace {

DimRefreshFn gRefresh = nullptr;

bool near(Vec2 a, Vec2 b) {
    return distance(a, b) <= 1e-7 * (1.0 + std::abs(a.x) + std::abs(a.y));
}

Vec2 arcEnd(const ArcData& a, bool end) {
    const double t = end ? a.endAngle : a.startAngle;
    return a.center + Vec2{std::cos(t), std::sin(t)} * a.radius;
}

}  // namespace

void setDimRefresh(DimRefreshFn fn) {
    gRefresh = fn;
}
DimRefreshFn dimRefresh() {
    return gRefresh;
}

std::optional<Vec2> dimAssocPoint(const Entity& e, const DimAssoc& a) {
    if (a.kind == DimAssoc::Kind::Center) {
        if (const auto* c = entityAs<CircleData>(e))
            return c->center;
        if (const auto* r = entityAs<ArcData>(e))
            return r->center;
        return std::nullopt;
    }
    if (a.kind == DimAssoc::Kind::OnCurve) {
        const double t = static_cast<double>(a.index) / 4294967296.0 * kTwoPi;
        const Vec2 dir{std::cos(t), std::sin(t)};
        if (const auto* c = entityAs<CircleData>(e))
            return c->center + dir * c->radius;
        if (const auto* r = entityAs<ArcData>(e))
            return r->center + dir * r->radius;
        return std::nullopt;
    }
    if (a.kind != DimAssoc::Kind::Vertex)
        return std::nullopt;
    if (const auto* l = entityAs<LineData>(e))
        return a.index == 0 ? std::optional<Vec2>(l->a)
                            : (a.index == 1 ? std::optional<Vec2>(l->b) : std::nullopt);
    if (const auto* p = entityAs<PolylineData>(e))
        return a.index < p->v.size() ? std::optional<Vec2>(p->v[a.index].p) : std::nullopt;
    if (const auto* r = entityAs<ArcData>(e))
        return a.index <= 1 ? std::optional<Vec2>(arcEnd(*r, a.index == 1)) : std::nullopt;
    if (const auto* pt = entityAs<PointData>(e))
        return pt->p;
    return std::nullopt;
}

std::optional<DimAssoc> findDimAssoc(const Document& doc, Vec2 p, BlockId owner) {
    std::optional<DimAssoc> found;
    doc.forEachEntity([&](const Entity& e) {
        if (found || doc.ownerBlock(e.common.id) != owner)
            return;
        auto test = [&](DimAssoc a) {
            if (found)
                return;
            a.target = e.common.id;
            if (auto q = dimAssocPoint(e, a); q && near(*q, p))
                found = a;
        };
        using K = DimAssoc::Kind;
        if (entityAs<CircleData>(e)) {
            test({K::Center, {}, 0});
        } else if (const auto* l = entityAs<LineData>(e)) {
            (void)l;
            test({K::Vertex, {}, 0});
            test({K::Vertex, {}, 1});
        } else if (const auto* pl = entityAs<PolylineData>(e)) {
            for (std::uint32_t i = 0; i < pl->v.size() && !found; ++i)
                test({K::Vertex, {}, i});
        } else if (entityAs<ArcData>(e)) {
            test({K::Center, {}, 0});
            test({K::Vertex, {}, 0});
            test({K::Vertex, {}, 1});
        } else if (entityAs<PointData>(e)) {
            test({K::Vertex, {}, 0});
        }
    });
    return found;
}

std::optional<DimAssoc> findDimAssocOnCurve(const Document& doc, Vec2 p, BlockId owner) {
    std::optional<DimAssoc> found;
    doc.forEachEntity([&](const Entity& e) {
        if (found || doc.ownerBlock(e.common.id) != owner)
            return;
        Vec2 c{};
        double r = 0;
        if (const auto* ci = entityAs<CircleData>(e)) {
            c = ci->center;
            r = ci->radius;
        } else if (const auto* ar = entityAs<ArcData>(e)) {
            c = ar->center;
            r = ar->radius;
        } else {
            return;
        }
        const Vec2 v = p - c;
        if (!(r > 0) || std::abs(length(v) - r) > 1e-7 * (1.0 + r + std::abs(p.x) + std::abs(p.y)))
            return;
        const double t = normalizeAngle(std::atan2(v.y, v.x));
        DimAssoc a;
        a.kind = DimAssoc::Kind::OnCurve;
        a.target = e.common.id;
        a.index =
            static_cast<std::uint32_t>(std::llround(t / kTwoPi * 4294967296.0) & 0xFFFFFFFFll);
        found = a;
    });
    return found;
}

int linkDimension(const Document& doc, DimensionData& d, BlockId owner) {
    int n = 0;
    std::size_t count = 0;
    switch (d.kind) {
        case DimKind::Linear:
        case DimKind::Aligned:
            count = 2;
            break;
        case DimKind::Angular2Line:
            count = 4;
            break;
        case DimKind::Angular3Point:
            count = 3;
            break;
        case DimKind::Radius:
        case DimKind::Diameter: {
            // The centre (radius) and the points on the circle or arc follow it, as in AutoCAD.
            for (std::size_t i = 0; i < 2; ++i) {
                d.assoc[i] = {};
                std::optional<DimAssoc> a;
                if (i == 0 && d.kind == DimKind::Radius)
                    a = findDimAssoc(doc, d.defPts[0], owner);
                if (!a)
                    a = findDimAssocOnCurve(doc, d.defPts[i], owner);
                if (a) {
                    d.assoc[i] = *a;
                    ++n;
                }
            }
            return n;
        }
        default:
            return 0;
    }
    for (std::size_t i = 0; i < count; ++i) {
        d.assoc[i] = {};
        if (auto a = findDimAssoc(doc, d.defPts[i], owner)) {
            d.assoc[i] = *a;
            ++n;
        }
    }
    return n;
}

}  // namespace tbcad
