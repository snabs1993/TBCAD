#include <cmath>
#include <cstdio>
#include <iterator>

#include "tbcad/geom/geom.h"
#include "tbcad/inquiry/inquiry.h"

namespace tbcad::inquiry {

namespace {

double shoelace(const std::vector<Vec2>& pts) {
    // Measured from the first point: with survey-sized coordinates (1e9 mm) the products of
    // absolute coordinates would have lost every digit of an area of a few thousand mm2.
    double s = 0;
    if (pts.empty())
        return 0;
    for (std::size_t i = 0; i < pts.size(); ++i) {
        const Vec2 a = pts[i] - pts[0];
        const Vec2 b = pts[(i + 1) % pts.size()] - pts[0];
        s += cross(a, b);
    }
    return std::abs(s) / 2;
}

double closedLength(const std::vector<Vec2>& pts) {
    double len = 0;
    for (std::size_t i = 0; i < pts.size(); ++i)
        len += distance(pts[i], pts[(i + 1) % pts.size()]);
    return len;
}

std::string fixed(double v, Precision p) {
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.*f", std::clamp(p, 0, 16), v);
    return buf;
}

std::string pt(const Document& doc, Vec2 p, Precision prec) {
    return "(" + formatLengthIn(doc, p.x, prec) + ", " + formatLengthIn(doc, p.y, prec) + ")";
}

}  // namespace

std::optional<AreaResult> areaOfEntity(const Entity& e) {
    if (const auto* c = entityAs<CircleData>(e))
        return AreaResult{kPi * c->radius * c->radius, kTwoPi * c->radius};
    if (const auto* el = entityAs<EllipseData>(e)) {
        double sweep = el->endParam - el->startParam;
        if (std::abs(std::abs(sweep) - kTwoPi) > 1e-9 && std::abs(sweep) > 1e-9)
            return std::nullopt;
        double a = length(el->majorAxis);
        double b = a * el->ratio;
        auto curve = geom::CurveRef::ellipse(el->center, el->majorAxis, el->ratio, 0, kTwoPi);
        return AreaResult{kPi * a * b, curve.length()};
    }
    if (const auto* pl = entityAs<PolylineData>(e)) {
        if (pl->v.size() < 2)
            return std::nullopt;
        return AreaResult{geom::area(*pl), geom::length(*pl)};
    }
    if (const auto* sp = entityAs<SplineData>(e)) {
        if (!sp->closed && !sp->periodic)
            return std::nullopt;
        auto curve = geom::CurveRef::spline(*sp);
        if (!curve)
            return std::nullopt;
        std::vector<Vec2> pts;
        const BBox2 box = curve.value().bounds();
        geom::outlinePoints(e, std::clamp(distance(box.min, box.max) * 1e-9, 1e-10, 1e-5), pts);
        if (pts.size() > 1 && distance(pts.front(), pts.back()) < 1e-9)
            pts.pop_back();
        if (pts.size() < 3)
            return std::nullopt;
        return AreaResult{shoelace(pts), curve.value().length()};
    }
    return std::nullopt;
}

AreaResult areaOfPoints(const std::vector<Vec2>& pts) {
    if (pts.size() < 2)
        return {};
    return {shoelace(pts), closedLength(pts)};
}

double angleAtVertex(Vec2 vertex, Vec2 a, Vec2 b) {
    Vec2 u = a - vertex, v = b - vertex;
    double lu = length(u), lv = length(v);
    if (lu < tol::kLinear || lv < tol::kLinear)
        return 0;
    double c = dot(u, v) / (lu * lv);
    double s = std::abs(cross(u, v)) / (lu * lv);
    return std::atan2(s, c);
}

double angleBetweenLines(Vec2 a0, Vec2 a1, Vec2 pickA, Vec2 b0, Vec2 b1, Vec2 pickB) {
    Vec2 da = a1 - a0, db = b1 - b0;
    double den = cross(da, db);
    Vec2 ra, rb;
    if (std::abs(den) > 1e-12 * length(da) * length(db)) {
        double t = cross(b0 - a0, db) / den;
        Vec2 x = a0 + da * t;
        ra = pickA - x;
        rb = pickB - x;
        if (length(ra) < tol::kLinear)
            ra = da;
        if (length(rb) < tol::kLinear)
            rb = db;
    } else {
        // Parallel: use each line's direction pointing towards its pick from its first end.
        ra = dot(pickA - a0, da) >= 0 ? da : -da;
        rb = dot(pickB - b0, db) >= 0 ? db : -db;
    }
    return angleAtVertex({0, 0}, ra, rb);
}

std::string formatLengthIn(const Document& doc, double mm, Precision precision) {
    UnitFormat f;
    f.unit = doc.header().units;
    f.showSuffix = true;
    return formatLength(mm, precision, f);
}

std::string formatAreaIn(const Document& doc, double mm2, Precision precision) {
    LengthUnit u = doc.header().units;
    double k = mmPerUnit(u);
    return fixed(mm2 / (k * k), precision) + " " + std::string(unitSuffix(u)) + "^2";
}

std::vector<std::string> describeEntity(const Document& doc, const Entity& e, Precision prec) {
    std::vector<std::string> out;
    static const char* const kNames[] = {
        "LINE",   "CIRCLE", "ARC",   "LWPOLYLINE", "POINT",  "ELLIPSE",
        "SPLINE", "TEXT",   "MTEXT", "HATCH",      "INSERT", "DIMENSION",
        "LEADER", "PROXY",  "TABLE", "RAY",        "MLINE",  "MULTILEADER"};
    static_assert(std::size(kNames) == kEntityKindCount, "a name for every entity kind");
    const auto idx = static_cast<std::size_t>(kind(e));
    const auto* ray = entityAs<RayData>(e);
    const std::string name = ray && ray->infinite ? "XLINE" : kNames[idx];
    out.push_back(name + "  Handle = " + std::to_string(e.common.id.value));
    const Layer* layer = doc.layer(e.common.layer);
    out.push_back("  Layer: " + (layer ? layer->name : std::string("?")));
    auto colorName = [](const Color& c) -> std::string {
        switch (c.kind) {
            case ColorKind::ByLayer:
                return "BYLAYER";
            case ColorKind::ByBlock:
                return "BYBLOCK";
            case ColorKind::ByParent:
                return "BYPARENT";
            case ColorKind::Index:
                return std::to_string(c.indexValue());
            case ColorKind::Rgb:
                return std::to_string(c.r) + "," + std::to_string(c.g) + "," + std::to_string(c.b);
        }
        return "?";
    };
    out.push_back("  Color: " + colorName(e.common.color));
    const Linetype* lt = doc.linetype(e.common.linetype);
    out.push_back("  Linetype: " + (e.common.linetype == kLinetypeByLayer
                                        ? std::string("BYLAYER")
                                        : (lt ? lt->name : std::string("?"))));

    auto L = [&](double v) { return formatLengthIn(doc, v, prec); };
    auto A = [&](double v) { return formatAreaIn(doc, v, prec); };
    auto ang = [&](double rad) { return formatAngle(rad, prec, AngleUnit::Degrees, true); };

    visitEntity(e, [&](const auto& d) {
        using T = std::decay_t<decltype(d)>;
        if constexpr (std::is_same_v<T, LineData>) {
            out.push_back("  From " + pt(doc, d.a, prec));
            out.push_back("  To   " + pt(doc, d.b, prec));
            out.push_back("  Length = " + L(distance(d.a, d.b)) + ", Angle = " +
                          ang(normalizeAngle(std::atan2(d.b.y - d.a.y, d.b.x - d.a.x))));
        } else if constexpr (std::is_same_v<T, CircleData>) {
            out.push_back("  Center " + pt(doc, d.center, prec));
            out.push_back("  Radius = " + L(d.radius));
            out.push_back("  Circumference = " + L(kTwoPi * d.radius));
            out.push_back("  Area = " + A(kPi * d.radius * d.radius));
        } else if constexpr (std::is_same_v<T, ArcData>) {
            double sweep = normalizeAngle(d.endAngle - d.startAngle);
            out.push_back("  Center " + pt(doc, d.center, prec));
            out.push_back("  Radius = " + L(d.radius));
            out.push_back("  Start angle = " + ang(d.startAngle) +
                          ", End angle = " + ang(d.endAngle));
            out.push_back("  Length = " + L(d.radius * sweep));
        } else if constexpr (std::is_same_v<T, PolylineData>) {
            out.push_back(std::string("  ") + (d.closed ? "Closed" : "Open") + ", " +
                          std::to_string(d.v.size()) + " vertices");
            for (const auto& v : d.v) {
                std::string s = "  Vertex " + pt(doc, v.p, prec);
                if (std::abs(v.bulge) > 1e-12)
                    s += ", bulge = " + fixed(v.bulge, 4);
                out.push_back(std::move(s));
            }
            out.push_back("  Length = " + L(geom::length(d)));
            if (d.closed)
                out.push_back("  Area = " + A(geom::area(d)));
        } else if constexpr (std::is_same_v<T, PointData>) {
            out.push_back("  At " + pt(doc, d.p, prec));
        } else if constexpr (std::is_same_v<T, EllipseData>) {
            out.push_back("  Center " + pt(doc, d.center, prec));
            out.push_back("  Major axis " + pt(doc, d.majorAxis, prec) +
                          ", ratio = " + fixed(d.ratio, 4));
            if (auto r = areaOfEntity(e))
                out.push_back("  Area = " + A(r->area) + ", Perimeter = " + L(r->perimeter));
        } else if constexpr (std::is_same_v<T, SplineData>) {
            out.push_back(std::string("  Degree ") + std::to_string(d.degree) + ", " +
                          std::to_string(d.control.size()) + " control points" +
                          (d.closed ? ", closed" : ""));
            out.push_back("  Length = " + L(geom::length(e)));
        } else if constexpr (std::is_same_v<T, TextData>) {
            out.push_back("  At " + pt(doc, d.position, prec) + ", height = " + L(d.height));
            out.push_back("  Text: " + d.utf8);
        } else if constexpr (std::is_same_v<T, MTextData>) {
            out.push_back("  At " + pt(doc, d.position, prec) + ", height = " + L(d.height));
            out.push_back("  Text: " + d.utf8);
        } else if constexpr (std::is_same_v<T, HatchData>) {
            out.push_back(std::string("  ") + (d.solid ? "Solid" : "Pattern " + d.pattern) + ", " +
                          std::to_string(d.loops.size()) + " loops");
        } else if constexpr (std::is_same_v<T, InsertData>) {
            const BlockDef* def = doc.block(d.block);
            out.push_back("  Block: " + (def ? def->name : std::string("?")) +
                          (def && !def->xrefPath.empty()
                               ? std::string("  (external reference ") + def->xrefPath +
                                     (def->xrefOverlay ? ", overlay)" : ")")
                               : std::string()));
            out.push_back("  At " + pt(doc, d.position, prec) + ", rotation = " + ang(d.rotation));
            out.push_back("  Scale X = " + fixed(d.scale.x, 4) + ", Y = " + fixed(d.scale.y, 4));
            if (def)
                out.push_back("  Contains " + std::to_string(def->entities.size()) + " objects");
        } else if constexpr (std::is_same_v<T, DimensionData>) {
            out.push_back("  Measurement = " + fixed(d.measurement, prec));
        } else if constexpr (std::is_same_v<T, LeaderData>) {
            out.push_back("  " + std::to_string(d.vertices.size()) + " vertices");
        } else if constexpr (std::is_same_v<T, MLeaderData>) {
            out.push_back("  " + std::to_string(d.lines.size()) + " leader lines");
        } else if constexpr (std::is_same_v<T, ProxyData>) {
            out.push_back("  Imported type: " + d.originalType);
        }
    });
    return out;
}

}  // namespace tbcad::inquiry
