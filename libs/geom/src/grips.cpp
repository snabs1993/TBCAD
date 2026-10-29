#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>

#include "internal.h"
#include "tbcad/geom/entity.h"
#include "tbcad/geom/revcloud.h"

namespace tbcad::geom {

namespace {

Error badGrip() {
    return makeError(ErrorCode::OutOfRange, "grip index out of range");
}

// The two ends of the dimension line of a linear or aligned dimension, where the extension lines
// (leaning by the oblique angle) meet it, and the line's direction.
struct DimLineEnds {
    Vec2 a{}, b{}, u{};
};
DimLineEnds dimLineEnds(const DimensionData& d) {
    Vec2 u = d.kind == DimKind::Linear ? Vec2{std::cos(d.rotation), std::sin(d.rotation)}
                                       : normalized(d.defPts[1] - d.defPts[0]);
    if (tol::isZeroLength(u))
        u = {1, 0};
    const Vec2 n = perp(u);
    const Vec2 e =
        d.oblique == 0 ? n : normalized(n * std::cos(d.oblique) - u * std::sin(d.oblique));
    const double lean = dot(e, n);
    const Vec2 P = d.defPts[2];
    return {d.defPts[0] + e * (dot(P - d.defPts[0], n) / lean),
            d.defPts[1] + e * (dot(P - d.defPts[1], n) / lean), u};
}

bool linearDim(const DimensionData& d) {
    return d.kind == DimKind::Linear || d.kind == DimKind::Aligned;
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
    const Vec2 first =
        out.center +
        Vec2{std::cos(r.value().startAngle), std::sin(r.value().startAngle)} * out.radius;
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
                Vec2 s =
                    a->center + Vec2{std::cos(a->startAngle), std::sin(a->startAngle)} * a->radius;
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

// ---- associative arrays ------------------------------------------------------------------------
// As AutoCAD, an array insert has grips besides its base (index 0) that change its parameters.
// Rectangular: 1 column count (at the last column), 2 column spacing (second column), 3 row count,
// 4 row spacing. Polar: 1 angle between items (at the second item), 2 fill angle (at the last
// item). Grips sit where the block's origin lands in that item: array blocks have their base at
// the origin, so a grip is the insert's position plus the item offset turned and scaled with it.

Vec2 insertVector(const InsertData& d, Vec2 v) {
    const Vec2 s{v.x * d.scale.x, v.y * d.scale.y};
    const double c = std::cos(d.rotation), sn = std::sin(d.rotation);
    return {s.x * c - s.y * sn, s.x * sn + s.y * c};
}

std::optional<Vec2> blockVector(const InsertData& d, Vec2 w) {
    if (d.scale.x == 0 || d.scale.y == 0)
        return std::nullopt;
    const double c = std::cos(d.rotation), sn = std::sin(d.rotation);
    const Vec2 r{w.x * c + w.y * sn, -w.x * sn + w.y * c};
    return Vec2{r.x / d.scale.x, r.y / d.scale.y};
}

double polarStep(const ArrayInfo& a) {
    const bool full = std::abs(std::abs(a.fillAngle) - kTwoPi) < 1e-9;
    const int cnt = std::max(a.count, 1);
    return cnt < 2 ? 0 : (full ? a.fillAngle / cnt : a.fillAngle / (cnt - 1));
}

// The path of a path array, walked by arc length.
struct ArrayPath {
    std::vector<Segment> segs;
    std::vector<double> lens;
    double total = 0;
    explicit ArrayPath(const ArrayInfo& a) {
        PolylineData pl;
        pl.v = a.path;
        pl.closed = a.pathClosed;
        segs = segments(pl);
        for (const Segment& g : segs) {
            lens.push_back(g.curve.length());
            total += lens.back();
        }
    }
    Vec2 pointAt(double s) const {
        for (std::size_t i = 0; i < segs.size(); ++i) {
            if (s <= lens[i] || i + 1 == segs.size()) {
                const double u = lens[i] > 0 ? std::clamp(s / lens[i], 0.0, 1.0) : 0.0;
                return segs[i].pointAtLocal(u);
            }
            s -= lens[i];
        }
        return {};
    }
    // Arc length from the start to the point of the path nearest p.
    double lengthAt(Vec2 p) const {
        double best = std::numeric_limits<double>::infinity(), at = 0, run = 0;
        for (std::size_t i = 0; i < segs.size(); ++i) {
            const double t = segs[i].curve.paramAt(p);
            const double d = distance(segs[i].curve.pointAt(t), p);
            if (d < best) {
                best = d;
                at = run + lens[i] * segs[i].toLocal(t);
            }
            run += lens[i];
        }
        return at;
    }
    // Distance between items as arrayItemTransforms spaces them.
    double spacing(const ArrayInfo& a) const {
        if (a.pathSpacing > 0)
            return a.pathSpacing;
        const int cnt = std::max(a.count, 1);
        return a.pathClosed ? total / cnt : (cnt > 1 ? total / (cnt - 1) : 0);
    }
    // Where item i sits (its copy of the reference point).
    Vec2 item(const ArrayInfo& a, int i) const {
        double s = spacing(a) * i;
        s = a.pathClosed && total > 0 ? std::fmod(s, total) : std::min(s, total);
        return pointAt(s);
    }
};

void arrayGrips(const InsertData& d, std::vector<Vec2>& g) {
    const ArrayInfo& a = *d.array;
    auto at = [&](Vec2 blockOffset) { return d.position + insertVector(d, blockOffset); };
    if (a.kind == ArrayInfo::Kind::Rectangular) {
        const Vec2 ex{std::cos(a.angle), std::sin(a.angle)};
        const Vec2 ey = perp(ex);
        g.push_back(at(ex * (a.columnSpacing * (std::max(a.columns, 1) - 1))));
        g.push_back(at(ex * a.columnSpacing));
        g.push_back(at(ey * (a.rowSpacing * (std::max(a.rows, 1) - 1))));
        g.push_back(at(ey * a.rowSpacing));
    } else if (a.kind == ArrayInfo::Kind::Polar) {
        // the item's copy of the reference point (where the source sits) around the centre
        const double step = polarStep(a);
        auto item = [&](double ang) {
            const Vec2 r = a.reference - a.center;
            const double c = std::cos(ang), sn = std::sin(ang);
            return at(a.center + Vec2{r.x * c - r.y * sn, r.x * sn + r.y * c});
        };
        g.push_back(item(step));
        g.push_back(item(step * (std::max(a.count, 1) - 1)));
    } else if (a.kind == ArrayInfo::Kind::Path && a.path.size() >= 2) {
        const ArrayPath path(a);
        if (path.segs.empty())
            return;
        g.push_back(at(path.item(a, 1)));
        g.push_back(at(path.item(a, std::max(a.count, 1) - 1)));
    }
}

// Moves array grip `index` (1 and up) of `d` to `to`.
Result<void> moveArrayGrip(InsertData& d, int index, Vec2 to) {
    ArrayInfo a = *d.array;
    const auto v = blockVector(d, to - d.position);
    if (!v)
        return makeError(ErrorCode::InvalidArgument, "the array is scaled to nothing");
    constexpr long long kMaxItems = 100000;
    if (a.kind == ArrayInfo::Kind::Rectangular && index >= 1 && index <= 4) {
        const Vec2 ex{std::cos(a.angle), std::sin(a.angle)};
        const Vec2 ey = perp(ex);
        const bool cols = index <= 2;
        const double along = dot(*v, cols ? ex : ey);
        double& spacing = cols ? a.columnSpacing : a.rowSpacing;
        int& count = cols ? a.columns : a.rows;
        if (index == 2 || index == 4) {  // spacing
            if (std::abs(along) < tol::kLinear)
                return makeError(ErrorCode::InvalidArgument, "the spacing would be zero");
            spacing = along;
        } else {  // count: as many items as fit up to the grip
            if (std::abs(spacing) < tol::kLinear)
                return makeError(ErrorCode::InvalidArgument, "the spacing is zero");
            const double n = std::floor(along / spacing + 0.5) + 1;
            count = static_cast<int>(std::clamp(n, 1.0, static_cast<double>(kMaxItems)));
        }
        if (static_cast<long long>(std::max(a.rows, 1)) * std::max(a.columns, 1) > kMaxItems)
            return makeError(ErrorCode::InvalidArgument, "that array would have too many items");
    } else if (a.kind == ArrayInfo::Kind::Polar && (index == 1 || index == 2)) {
        const Vec2 r = a.reference - a.center, q = *v - a.center;
        if (tol::isZeroLength(r) || tol::isZeroLength(q))
            return makeError(ErrorCode::InvalidArgument, "the grip is on the centre");
        // counter-clockwise angle from the source to the grip, in (0, 2 pi]
        double ang = std::atan2(cross(r, q), dot(r, q));
        if (ang <= 1e-12)
            ang += kTwoPi;
        const bool full = std::abs(std::abs(a.fillAngle) - kTwoPi) < 1e-9;
        const int cnt = std::max(a.count, 1);
        if (index == 1) {  // angle between items
            if (full) {
                // the items stay spread round the circle: as many as the new angle allows
                a.count = static_cast<int>(std::clamp(std::floor(kTwoPi / ang + 0.5), 2.0,
                                                      static_cast<double>(kMaxItems)));
            } else {
                a.fillAngle = std::min(ang * (cnt - 1), kTwoPi);
            }
        } else {  // fill angle: the last item goes to the grip
            if (cnt < 2)
                return makeError(ErrorCode::InvalidArgument, "a single item has no fill angle");
            a.fillAngle = ang;
        }
    } else if (a.kind == ArrayInfo::Kind::Path && (index == 1 || index == 2) &&
               a.path.size() >= 2) {
        const ArrayPath path(a);
        if (path.segs.empty() || path.total <= tol::kLinear)
            return makeError(ErrorCode::InvalidArgument, "the path has no length");
        // Both grips measure along the path from its start (where the first item sits); the
        // items are then spaced by distance (Measure).
        const double s = path.lengthAt(*v);
        if (index == 1) {  // the second item: the spacing
            if (s <= tol::kLinear)
                return makeError(ErrorCode::InvalidArgument, "the spacing would be zero");
            a.pathSpacing = s;
        } else {  // the last item: as many items as fit up to the grip
            const double sp = path.spacing(a);
            if (sp <= tol::kLinear)
                return makeError(ErrorCode::InvalidArgument, "the spacing is zero");
            a.pathSpacing = sp;
            a.count = static_cast<int>(
                std::clamp(std::floor(s / sp + 0.5) + 1, 1.0, static_cast<double>(kMaxItems)));
        }
    } else {
        return badGrip();
    }
    carryArrayOverrides(*d.array, a);
    d.array = a;
    return {};
}

}  // namespace

int dimTextGrip(const DimensionData& d) {
    switch (d.kind) {
        case DimKind::Radius:
        case DimKind::Diameter:
            return 2;  // centre (or first end) and the point on the circle
        case DimKind::Ordinate:
            return 2;  // feature point and leader end (the origin is not dragged)
        default:
            return 4;
    }
}

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
            if (const auto cloud = cloudShape(e)) {
                g = cloudGrips(*cloud);
                break;
            }
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
        case EntityKind::Insert: {
            const auto& d = *entityAs<InsertData>(e);
            g = {d.position};
            if (d.array)
                arrayGrips(d, g);
            break;
        }
        case EntityKind::Dimension: {
            // As in AutoCAD: the extension line origins and the ends of the dimension line of a
            // linear dimension; the points that place other kinds; then the text.
            const auto& d = *entityAs<DimensionData>(e);
            if (linearDim(d)) {
                const DimLineEnds ends = dimLineEnds(d);
                g = {d.defPts[0], d.defPts[1], ends.a, ends.b};
            } else {
                g.assign(d.defPts.begin(), d.defPts.begin() + dimTextGrip(d));
            }
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
            if (const auto cloud = cloudShape(e))  // the outline moves, the arcs are laid again
                return moveCloudGrip(e, *cloud, index, to);
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
        case EntityKind::Insert: {
            auto& d = *entityAs<InsertData>(out);
            if (index == 0) {
                d.position = to;
                return out;
            }
            if (!d.array)
                return badGrip();
            if (auto r = moveArrayGrip(d, index, to); !r)
                return r.error();
            return out;
        }
        case EntityKind::Dimension: {
            auto& d = *entityAs<DimensionData>(out);
            const int text = dimTextGrip(d);
            if (index < 0 || index > text)
                return badGrip();
            if (index == text) {
                d.textPos = to;
                d.textPosUser = true;
            } else if (linearDim(d) && index >= 2) {
                // A dimension line end moves the whole line (sideways only).
                const Vec2 n = perp(dimLineEnds(d).u);
                d.defPts[2] = d.defPts[2] + n * dot(to - d.defPts[2], n);
            } else {
                d.defPts[static_cast<std::size_t>(index)] = to;
            }
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

void carryArrayOverrides(const ArrayInfo& from, ArrayInfo& to) {
    const bool grid =
        from.kind == ArrayInfo::Kind::Rectangular && to.kind == ArrayInfo::Kind::Rectangular;
    const int fromCols = std::max(from.columns, 1);
    const int toCols = std::max(to.columns, 1);
    const long long count = to.kind == ArrayInfo::Kind::Rectangular
                                ? static_cast<long long>(std::max(to.rows, 1)) * toCols
                                : std::max(to.count, 1);
    // The item's new index, or -1 when it is no longer in the array. Increasing, so the lists
    // stay sorted.
    auto map = [&](int i) -> long long {
        long long n = i;
        if (grid) {
            const int r = i / fromCols, c = i % fromCols;
            if (r >= std::max(to.rows, 1) || c >= toCols)
                return -1;
            n = static_cast<long long>(r) * toCols + c;
        }
        return n >= 0 && n < count ? n : -1;
    };
    to.removed.clear();
    to.swaps.clear();
    for (int i : from.removed)
        if (long long n = map(i); n >= 0)
            to.removed.push_back(static_cast<int>(n));
    for (ArrayItemSwap sw : from.swaps)
        if (long long n = map(sw.item); n >= 0) {
            sw.item = static_cast<int>(n);
            to.swaps.push_back(sw);
        }
}

}  // namespace tbcad::geom
