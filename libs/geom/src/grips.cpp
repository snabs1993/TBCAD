#include <algorithm>
#include <cmath>

#include "internal.h"
#include "tbcad/geom/entity.h"

namespace tbcad::geom {

namespace {

Error badGrip() {
    return makeError(ErrorCode::OutOfRange, "grip index out of range");
}


// ---- hatch boundary ---------------------------------------------------------------------------
// A hatch has a grip on each corner of its boundary: ends of line and arc edges and the vertices
// of polyline edges, where edges meet being one grip. Ellipse and spline edges have none.

constexpr double kJoin = 1e-9;

HatchArcEdge hatchArcFromEnds(const HatchArcEdge& old, Vec2 start, Vec2 end, bool& ok) {
    ok = false;
    // The arc's middle, from the old edge: a ccw edge sweeps start -> end, a cw one the reverse.
    double sweep = old.endAngle - old.startAngle;
    if (old.ccw) {
        sweep = std::fmod(sweep, kTwoPi);
        if (sweep <= 0)
            sweep += kTwoPi;
    } else {
        sweep = std::fmod(sweep, kTwoPi);
        if (sweep >= 0)
            sweep -= kTwoPi;
    }
    const double midAngle = old.startAngle + sweep * 0.5;
    const Vec2 mid = old.center + Vec2{std::cos(midAngle), std::sin(midAngle)} * old.radius;
    auto r = arcFromThreePoints(start, mid, end);
    if (!r)
        return old;
    HatchArcEdge out = old;
    out.center = r.value().center;
    out.radius = r.value().radius;
    out.startAngle = std::atan2(start.y - out.center.y, start.x - out.center.x);
    out.endAngle = std::atan2(end.y - out.center.y, end.x - out.center.x);
    // arcFromThreePoints runs counter-clockwise from its first to its last point when the three
    // go that way round; when they go clockwise the arc it names starts at the last point.
    const Vec2 first = out.center + Vec2{std::cos(r.value().startAngle),
                                         std::sin(r.value().startAngle)} * out.radius;
    out.ccw = distance(first, start) <= distance(first, end);
    ok = true;
    return out;
}

std::vector<Vec2> hatchCorners(const HatchData& h) {
    std::vector<Vec2> out;
    auto add = [&](Vec2 p) {
        for (const Vec2& q : out)
            if (distance(p, q) <= kJoin)
                return;
        out.push_back(p);
    };
    for (const HatchLoop& loop : h.loops)
        for (const HatchEdge& ed : loop.edges) {
            if (const auto* l = std::get_if<HatchLineEdge>(&ed)) {
                add(l->a);
                add(l->b);
            } else if (const auto* a = std::get_if<HatchArcEdge>(&ed)) {
                add(a->center + Vec2{std::cos(a->startAngle), std::sin(a->startAngle)} * a->radius);
                add(a->center + Vec2{std::cos(a->endAngle), std::sin(a->endAngle)} * a->radius);
            } else if (const auto* pl = std::get_if<HatchPolylineEdge>(&ed)) {
                for (const PolyVertex& v : pl->v)
                    add(v.p);
            }
        }
    return out;
}

bool moveHatchCorner(HatchData& h, Vec2 from, Vec2 to) {
    bool moved = false;
    for (HatchLoop& loop : h.loops)
        for (HatchEdge& ed : loop.edges) {
            if (auto* l = std::get_if<HatchLineEdge>(&ed)) {
                if (distance(l->a, from) <= kJoin) {
                    l->a = to;
                    moved = true;
                }
                if (distance(l->b, from) <= kJoin) {
                    l->b = to;
                    moved = true;
                }
            } else if (auto* a = std::get_if<HatchArcEdge>(&ed)) {
                Vec2 s = a->center + Vec2{std::cos(a->startAngle), std::sin(a->startAngle)} * a->radius;
                Vec2 e = a->center + Vec2{std::cos(a->endAngle), std::sin(a->endAngle)} * a->radius;
                const bool ms = distance(s, from) <= kJoin, me = distance(e, from) <= kJoin;
                if (ms || me) {
                    bool ok = false;
                    HatchArcEdge n = hatchArcFromEnds(*a, ms ? to : s, me ? to : e, ok);
                    if (ok) {
                        *a = n;
                        moved = true;
                    }
                }
            } else if (auto* pl = std::get_if<HatchPolylineEdge>(&ed)) {
                for (PolyVertex& v : pl->v)
                    if (distance(v.p, from) <= kJoin) {
                        v.p = to;
                        moved = true;
                    }
            }
        }
    return moved;
}

}  // namespace

std::vector<Vec2> gripPoints(const Entity& e) {
    std::vector<Vec2> g;
    switch (kind(e)) {
        case EntityKind::Line: {
            const auto& d = *entityAs<LineData>(e);
            g = {d.a, d.b, (d.a + d.b) * 0.5};
            break;
        }
        case EntityKind::Circle: {
            const auto& d = *entityAs<CircleData>(e);
            g = {d.center, d.center + Vec2{d.radius, 0}, d.center + Vec2{0, d.radius},
                 d.center + Vec2{-d.radius, 0}, d.center + Vec2{0, -d.radius}};
            break;
        }
        case EntityKind::Arc: {
            const auto& d = *entityAs<ArcData>(e);
            CurveRef c = detail::arcCurve(d);
            g = {c.startPoint(), c.endPoint(), c.pointAt(0.5 * (c.t0() + c.t1())), d.center};
            break;
        }
        case EntityKind::Polyline: {
            const auto& pl = *entityAs<PolylineData>(e);
            for (const PolyVertex& v : pl.v)
                g.push_back(v.p);
            int n = static_cast<int>(pl.v.size());
            for (int i = 0; i < segmentCount(pl); ++i) {
                const PolyVertex& a = pl.v[static_cast<std::size_t>(i)];
                const PolyVertex& b = pl.v[static_cast<std::size_t>((i + 1) % n)];
                Vec2 m = (a.p + b.p) * 0.5;
                if (auto arc = bulgeArc(a.p, b.p, a.bulge)) {
                    Segment s;
                    s.curve = CurveRef::arc(arc->center, arc->radius, arc->startAngle, arc->sweep);
                    s.reversed = arc->reversed;
                    m = s.pointAtLocal(0.5);
                }
                g.push_back(m);
            }
            break;
        }
        case EntityKind::Point:
            g = {entityAs<PointData>(e)->p};
            break;
        case EntityKind::Ellipse: {
            const auto& d = *entityAs<EllipseData>(e);
            Vec2 minor = perp(d.majorAxis) * d.ratio;
            g = {d.center, d.center + d.majorAxis, d.center + minor, d.center - d.majorAxis,
                 d.center - minor};
            CurveRef c = detail::ellipseCurve(d);
            if (!c.isClosed()) {
                g.push_back(c.startPoint());
                g.push_back(c.endPoint());
            }
            break;
        }
        case EntityKind::Spline: {
            const auto& d = *entityAs<SplineData>(e);
            g = d.fit.empty() ? d.control : d.fit;
            // A periodic spline repeats its first `degree` vertices at the end: one grip each.
            if (d.fit.empty() && d.periodic &&
                d.control.size() > static_cast<std::size_t>(d.degree))
                g.resize(d.control.size() - static_cast<std::size_t>(d.degree));
            break;
        }
        case EntityKind::Text: {
            // The grip sits where the text is anchored: the justification point of centred,
            // right or top text; aligned and fitted text has both ends.
            const auto& d = *entityAs<TextData>(e);
            if (d.h == HAlign::Aligned || d.h == HAlign::Fit)
                g = {d.position, d.alignPoint};
            else if (d.h == HAlign::Left && d.v == VAlign::Baseline)
                g = {d.position};
            else
                g = {d.alignPoint};
            break;
        }
        case EntityKind::MText: {
            const auto& d = *entityAs<MTextData>(e);
            g = {d.position};
            if (d.width > 0 && d.columns <= 1) {  // the wrapping width: drag the free side
                const int col = static_cast<int>(d.attach) % 3;  // 0 left, 1 centre, 2 right
                const double off = col == 0 ? d.width : col == 1 ? d.width * 0.5 : -d.width;
                g.push_back(d.position + Vec2{std::cos(d.rotation), std::sin(d.rotation)} * off);
            }
            break;
        }
        case EntityKind::Insert:
            g = {entityAs<InsertData>(e)->position};
            break;
        case EntityKind::Dimension: {
            const auto& d = *entityAs<DimensionData>(e);
            g.assign(d.defPts.begin(), d.defPts.end());
            g.push_back(d.textPos);
            break;
        }
        case EntityKind::Leader: {
            const auto& d = *entityAs<LeaderData>(e);
            g = d.vertices;
            g.push_back(d.textPos);
            break;
        }
        case EntityKind::MLeader: {
            const auto& d = *entityAs<MLeaderData>(e);
            for (const auto& line : d.lines)
                g.insert(g.end(), line.begin(), line.end());
            g.push_back(d.landing);
            g.push_back(d.contentPos);
            break;
        }
        case EntityKind::Ray:
            // The origin, and a handle on the line that turns it (kRayGripReach from the origin).
            g = {entityAs<RayData>(e)->origin,
                 entityAs<RayData>(e)->origin + entityAs<RayData>(e)->dir * kRayGripReach};
            break;
        case EntityKind::MLine:
            g = entityAs<MlineData>(e)->v;
            break;
        case EntityKind::Table: {
            const auto& d = *entityAs<TableData>(e);
            g.push_back(d.origin);
            double w = 0, h = 0;
            for (double x : d.colWidths)
                w += x;
            for (double y : d.rowHeights)
                h += y;
            const double c = std::cos(d.rotation), si = std::sin(d.rotation);
            // bottom-right corner in the rotated frame (y points down)
            g.push_back(d.origin + Vec2{w * c + h * si, w * si - h * c});
            // Column borders on the top edge, then row borders on the left edge; the outer
            // borders are the origin and the corner.
            double x = 0;
            for (std::size_t i = 0; i + 1 < d.colWidths.size(); ++i) {
                x += d.colWidths[i];
                g.push_back(d.origin + Vec2{x * c, x * si});
            }
            double y = 0;
            for (std::size_t i = 0; i + 1 < d.rowHeights.size(); ++i) {
                y += d.rowHeights[i];
                g.push_back(d.origin + Vec2{y * si, -y * c});
            }
            break;
        }
        case EntityKind::Hatch:
            g = hatchCorners(*entityAs<HatchData>(e));
            break;
        default:
            break;
    }
    return g;
}

Result<Entity> moveGrip(const Entity& e, int index, Vec2 to) {
    if (!isFinite(to))
        return makeError(ErrorCode::InvalidArgument, "grip target is not finite");
    Entity out = e;
    if (index < 0)
        return badGrip();
    switch (kind(e)) {
        case EntityKind::Line: {
            auto& d = *entityAs<LineData>(out);
            if (index == 0)
                d.a = to;
            else if (index == 1)
                d.b = to;
            else if (index == 2) {
                Vec2 delta = to - (d.a + d.b) * 0.5;
                d.a += delta;
                d.b += delta;
            } else
                return badGrip();
            return out;
        }
        case EntityKind::Circle: {
            auto& d = *entityAs<CircleData>(out);
            if (index == 0) {
                d.center = to;
            } else if (index <= 4) {
                double r = distance(d.center, to);
                if (tol::isZeroLength(r))
                    return makeError(ErrorCode::InvalidArgument, "radius would be zero");
                d.radius = r;
            } else
                return badGrip();
            return out;
        }
        case EntityKind::Arc: {
            const auto& d = *entityAs<ArcData>(e);
            CurveRef c = detail::arcCurve(d);
            Vec2 s = c.startPoint(), en = c.endPoint(), m = c.pointAt(0.5 * (c.t0() + c.t1()));
            Result<ArcData> r = badGrip();
            switch (index) {
                case 0:
                    r = arcFromThreePoints(to, m, en);
                    break;
                case 1:
                    r = arcFromThreePoints(s, m, to);
                    break;
                case 2:
                    r = arcFromThreePoints(s, to, en);
                    break;
                case 3: {
                    auto& o = *entityAs<ArcData>(out);
                    o.center = to;
                    return out;
                }
                default:
                    return badGrip();
            }
            if (!r)
                return r.error();
            *entityAs<ArcData>(out) = r.value();
            return out;
        }
        case EntityKind::Polyline: {
            auto& pl = *entityAs<PolylineData>(out);
            int n = static_cast<int>(pl.v.size());
            if (index < n) {
                pl.v[static_cast<std::size_t>(index)].p = to;
                return out;
            }
            int seg = index - n;
            if (seg >= segmentCount(pl))
                return badGrip();
            PolyVertex& a = pl.v[static_cast<std::size_t>(seg)];
            PolyVertex& b = pl.v[static_cast<std::size_t>((seg + 1) % n)];
            if (a.bulge == 0) {
                // A straight edge: its midpoint grip stretches the edge, both ends follow the
                // drag (dragging a rectangle's edge moves that edge), like AutoCAD.
                const Vec2 delta = to - (a.p + b.p) * 0.5;
                a.p = a.p + delta;
                b.p = b.p + delta;
                return out;
            }
            auto bu = bulgeThrough(a.p, to, b.p);
            a.bulge = bu ? *bu : 0.0;
            return out;
        }
        case EntityKind::Hatch: {
            auto& h = *entityAs<HatchData>(out);
            const std::vector<Vec2> corners = hatchCorners(*entityAs<HatchData>(e));
            if (index >= static_cast<int>(corners.size()))
                return badGrip();
            if (!moveHatchCorner(h, corners[static_cast<std::size_t>(index)], to))
                return makeError(ErrorCode::InvalidArgument, "the boundary cannot follow");
            return out;
        }
        case EntityKind::Point:
            if (index != 0)
                return badGrip();
            entityAs<PointData>(out)->p = to;
            return out;
        case EntityKind::Ellipse: {
            auto& d = *entityAs<EllipseData>(out);
            if (index == 0) {
                d.center = to;
                return out;
            }
            if (index >= 1 && index <= 4) {
                Vec2 v = to - d.center;
                if (index == 1 || index == 3) {
                    Vec2 major = index == 1 ? v : -v;
                    if (tol::isZeroLength(major))
                        return makeError(ErrorCode::InvalidArgument, "axis would be zero");
                    d.majorAxis = major;
                } else {
                    double minor = std::abs(dot(v, normalized(perp(d.majorAxis))));
                    double ratio = minor / length(d.majorAxis);
                    if (!(ratio > 1e-9))
                        return makeError(ErrorCode::InvalidArgument, "axis ratio would be zero");
                    // Keep the major axis the longer one.
                    if (ratio > 1)
                        return makeError(ErrorCode::InvalidArgument,
                                         "minor axis cannot exceed the major axis");
                    d.ratio = ratio;
                }
                return out;
            }
            if (index == 5 || index == 6) {
                CurveRef full = CurveRef::ellipse(d.center, d.majorAxis, d.ratio, 0, kTwoPi);
                if (!detail::ellipseCurve(*entityAs<EllipseData>(e)).isClosed()) {
                    double t = normalizeAngle(full.paramAt(to));
                    (index == 5 ? d.startParam : d.endParam) = t;
                    return out;
                }
            }
            return badGrip();
        }
        case EntityKind::Spline: {
            auto& d = *entityAs<SplineData>(out);
            auto& pts = d.fit.empty() ? d.control : d.fit;
            if (index >= static_cast<int>(pts.size()))
                return badGrip();
            if (d.fit.empty() && d.periodic &&
                d.control.size() > static_cast<std::size_t>(d.degree)) {
                const std::size_t shown = d.control.size() - static_cast<std::size_t>(d.degree);
                if (static_cast<std::size_t>(index) >= shown)
                    return badGrip();
                if (static_cast<std::size_t>(index) < static_cast<std::size_t>(d.degree))
                    pts[shown + static_cast<std::size_t>(index)] = to;
            }
            pts[static_cast<std::size_t>(index)] = to;
            if (!d.fit.empty()) {
                InterpolateOptions o;
                o.degree = d.degree;
                o.closed = d.closed;
                auto r = interpolateSpline(d.fit, o);
                if (!r)
                    return r.error();
                d.knots = r.value().knots;
                d.control = r.value().control;
                d.weights.clear();
            }
            return out;
        }
        case EntityKind::Text: {
            auto& d = *entityAs<TextData>(out);
            if (d.h == HAlign::Aligned || d.h == HAlign::Fit) {
                if (index == 0)
                    d.position = to;
                else if (index == 1)
                    d.alignPoint = to;
                else
                    return badGrip();
                return out;
            }
            if (index != 0)
                return badGrip();
            const bool anchored = !(d.h == HAlign::Left && d.v == VAlign::Baseline);
            const Vec2 delta = to - (anchored ? d.alignPoint : d.position);
            d.position += delta;
            d.alignPoint += delta;
            return out;
        }
        case EntityKind::MText: {
            auto& d = *entityAs<MTextData>(out);
            if (index == 1 && d.width > 0 && d.columns <= 1) {
                const double along = (to.x - d.position.x) * std::cos(d.rotation) +
                                     (to.y - d.position.y) * std::sin(d.rotation);
                const int col = static_cast<int>(d.attach) % 3;
                const double w = col == 0 ? along : col == 1 ? along * 2 : -along;
                if (!(w > 1e-9))
                    return makeError(ErrorCode::InvalidArgument, "the width would collapse");
                d.width = w;
                return out;
            }
            if (index != 0)
                return badGrip();
            d.position = to;
            return out;
        }
        case EntityKind::Insert:
            if (index != 0)
                return badGrip();
            entityAs<InsertData>(out)->position = to;
            return out;
        case EntityKind::Dimension: {
            auto& d = *entityAs<DimensionData>(out);
            if (index < 4) {
                d.defPts[static_cast<std::size_t>(index)] = to;
            } else if (index == 4) {
                d.textPos = to;
                d.textPosUser = true;
            } else
                return badGrip();
            return out;
        }
        case EntityKind::Leader: {
            auto& d = *entityAs<LeaderData>(out);
            int n = static_cast<int>(d.vertices.size());
            if (index < n)
                d.vertices[static_cast<std::size_t>(index)] = to;
            else if (index == n)
                d.textPos = to;
            else
                return badGrip();
            return out;
        }
        case EntityKind::MLeader: {
            auto& d = *entityAs<MLeaderData>(out);
            int at = index;
            for (auto& line : d.lines) {
                if (at < static_cast<int>(line.size())) {
                    line[static_cast<std::size_t>(at)] = to;
                    return out;
                }
                at -= static_cast<int>(line.size());
            }
            if (at == 0) {
                // The lines that ended at the landing follow it.
                for (auto& line : d.lines)
                    if (!line.empty() && distance(line.back(), d.landing) < 1e-9)
                        line.back() = to;
                d.landing = to;
            } else if (at == 1) {
                d.contentPos = to;
            } else {
                return badGrip();
            }
            return out;
        }
        case EntityKind::Ray: {
            auto& d = *entityAs<RayData>(out);
            if (index == 0) {
                d.origin = to;
                return out;
            }
            if (index != 1)
                return badGrip();
            if (tol::isZeroLength(to - d.origin))
                return makeError(ErrorCode::InvalidArgument, "direction would be zero");
            d.dir = normalized(to - d.origin);
            return out;
        }
        case EntityKind::MLine: {
            auto& d = *entityAs<MlineData>(out);
            if (index < 0 || static_cast<std::size_t>(index) >= d.v.size())
                return badGrip();
            d.v[static_cast<std::size_t>(index)] = to;
            return out;
        }
        case EntityKind::Table: {
            auto& d = *entityAs<TableData>(out);
            if (index == 0) {
                d.origin = to;
                return out;
            }
            if (index > 1) {
                // A border grip moves one column or row border: that column or row changes size,
                // the table grows or shrinks with it.
                const std::size_t cols = d.colWidths.size() > 0 ? d.colWidths.size() - 1 : 0;
                const std::size_t rows = d.rowHeights.size() > 0 ? d.rowHeights.size() - 1 : 0;
                const std::size_t k = static_cast<std::size_t>(index - 2);
                const double c = std::cos(d.rotation), si = std::sin(d.rotation);
                const Vec2 v = to - d.origin;
                if (k < cols) {
                    double before = 0;
                    for (std::size_t i = 0; i < k; ++i)
                        before += d.colWidths[i];
                    const double nw = v.x * c + v.y * si - before;
                    if (!(nw > 1e-9))
                        return makeError(ErrorCode::InvalidArgument, "column would collapse");
                    d.colWidths[k] = nw;
                    return out;
                }
                if (k - cols < rows) {
                    const std::size_t r = k - cols;
                    double before = 0;
                    for (std::size_t i = 0; i < r; ++i)
                        before += d.rowHeights[i];
                    const double nh = v.x * si - v.y * c - before;
                    if (!(nh > 1e-9))
                        return makeError(ErrorCode::InvalidArgument, "row would collapse");
                    d.rowHeights[r] = nh;
                    return out;
                }
                return badGrip();
            }
            if (index != 1)
                return badGrip();
            // The corner drags the whole grid: columns and rows keep their proportions.
            double w = 0, h = 0;
            for (double x : d.colWidths)
                w += x;
            for (double y : d.rowHeights)
                h += y;
            const double c = std::cos(d.rotation), si = std::sin(d.rotation);
            const Vec2 v = to - d.origin;
            const double nw = v.x * c + v.y * si;
            const double nh = v.x * si - v.y * c;
            if (!(nw > 1e-9) || !(nh > 1e-9) || !(w > 0) || !(h > 0))
                return makeError(ErrorCode::InvalidArgument, "table would collapse");
            for (double& x : d.colWidths)
                x *= nw / w;
            for (double& y : d.rowHeights)
                y *= nh / h;
            return out;
        }
        default:
            return makeError(ErrorCode::Unsupported, "entity has no grips");
    }
}

}  // namespace tbcad::geom
