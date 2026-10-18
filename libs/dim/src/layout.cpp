#include <algorithm>
#include <cmath>

#include "tbcad/dim/dim.h"
#include "tbcad/text/field.h"
#include "tbcad/text/text.h"

namespace tbcad::dim {

namespace {

constexpr double kDeg = kPi / 180.0;

struct Ctx {
    const Document& doc;
    const DimStyle& st;
    double tol;
    const TextStyle* tstyle;
    double th, arrow, gap, off, ext, dext;  // style values times the overall scale

    Ctx(const Document& d, const DimStyle& s, double t) : doc(d), st(s), tol(t) {
        const double k = s.overallScale > 0 ? s.overallScale : 1.0;
        tstyle = text::styleFor(d, s.textStyle);
        th = s.textHeight * k;
        arrow = s.arrowSize * k;
        gap = s.textGap * k;
        off = s.extLineOffset * k;
        ext = s.extLineExtension * k;
        dext = s.dimLineExtension * k;
    }
};

Vec2 rotated(Vec2 v, double a) {
    double c = std::cos(a), s = std::sin(a);
    return {v.x * c - v.y * s, v.x * s + v.y * c};
}
Vec2 unit(double a) {
    return {std::cos(a), std::sin(a)};
}
double angleOf(Vec2 v) {
    return std::atan2(v.y, v.x);
}

// Text rotation that reads left to right (or bottom to top): folded into (-pi/2, pi/2].
double readable(double a) {
    a = std::remainder(a, kTwoPi);
    if (a > kPi / 2 + 1e-9)
        a -= kPi;
    else if (a <= -kPi / 2 + 1e-9)
        a += kPi;
    return a;
}

// Half the extent of a text box (width x height, rotated by `rot`) along `dir`.
double halfExtent(Vec2 dir, double rot, double tw, double th) {
    Vec2 ax = unit(rot), ay = perp(ax);
    return std::abs(dot(dir, ax)) * tw / 2 + std::abs(dot(dir, ay)) * th / 2;
}

double textWidth(const Ctx& c, const std::string& s) {
    return s.empty() ? 0 : text::measureWidth(c.tstyle, s, c.th);
}

void addSeg(Layout& L, Vec2 a, Vec2 b, Role role, double tol) {
    if (distance(a, b) > tol)
        L.lines.push_back({{a, b}, role});
}

// Arrow whose tip is at `tip`; `body` is the unit vector from the tip towards the tail.
void addArrow(Layout& L, const Ctx& c, Vec2 tip, Vec2 body) {
    const double a = c.arrow;
    if (!(a > 0))
        return;
    switch (c.st.arrowType) {
        case ArrowType::ClosedFilled: {
            Vec2 tail = tip + body * a;
            Vec2 w = perp(body) * (a / 6);
            L.arrows.push_back({{tip, tail + w, tail - w}});
            break;
        }
        case ArrowType::Open30: {
            Layout::Line l;
            l.pts = {tip + rotated(body, 15 * kDeg) * a, tip, tip + rotated(body, -15 * kDeg) * a};
            L.lines.push_back(std::move(l));
            break;
        }
        case ArrowType::ArchTick: {
            Vec2 d = rotated(body, 45 * kDeg) * (a / 2);
            L.lines.push_back({{tip - d, tip + d}, Role::DimLine});
            break;
        }
        case ArrowType::Dot: {
            const double r = a / 2;
            int n = 16;
            if (r > c.tol * 2)
                n = std::clamp(static_cast<int>(std::ceil(kPi / std::acos(1 - c.tol / r))), 12, 64);
            Layout::Arrow ar;
            for (int i = 0; i < n; ++i)
                ar.ring.push_back(tip + unit(kTwoPi * i / n) * r);
            L.arrows.push_back(std::move(ar));
            break;
        }
        case ArrowType::None:
            break;
    }
}

// Where text sits relative to a dimension line: lifted above it, or on it (the line then breaks).
struct Lift {
    bool centered = false;
    Vec2 offset{};
};
Lift liftFor(const Ctx& c, Vec2 lineDir, double rotText) {
    Lift l;
    l.centered = c.st.textPlacement == DimTextPlacement::Centered ||
                 (c.st.textAlign == DimTextAlign::Horizontal && std::abs(lineDir.y) > 0.5);
    if (!l.centered)
        l.offset = perp(unit(rotText)) * (c.gap + c.th / 2);
    return l;
}

// ---- linear and aligned ------------------------------------------------------------------------

void layoutLinear(const Ctx& c, const DimensionData& d, const std::string& txt, Layout& L) {
    const Vec2 p1 = d.defPts[0], p2 = d.defPts[1], P = d.defPts[2];
    Vec2 u = d.kind == DimKind::Linear ? unit(d.rotation) : normalized(p2 - p1);
    if (tol::isZeroLength(u))
        u = {1, 0};
    const Vec2 n = perp(u);
    const double s1 = dot(P - p1, n), s2 = dot(P - p2, n);
    const Vec2 f1 = p1 + n * s1, f2 = p2 + n * s2;

    const Vec2 pts[2] = {p1, p2};
    const Vec2 feet[2] = {f1, f2};
    const double ss[2] = {s1, s2};
    for (int i = 0; i < 2; ++i) {
        double dist = std::abs(ss[i]);
        if (dist <= c.off)
            continue;
        double sg = ss[i] >= 0 ? 1 : -1;
        L.lines.push_back({{pts[i] + n * (sg * c.off), feet[i] + n * (sg * c.ext)}, Role::ExtLine});
    }

    const bool swap = dot(f1, u) > dot(f2, u);
    const Vec2 A = swap ? f2 : f1, B = swap ? f1 : f2;
    const double len = distance(A, B);
    const Vec2 uu = len > c.tol ? normalized(B - A) : u;

    const double tw = textWidth(c, txt);
    const double rotText = c.st.textAlign == DimTextAlign::Horizontal ? 0 : readable(angleOf(uu));
    const Lift lift = liftFor(c, uu, rotText);
    const double stub = 2 * c.arrow;
    const double textW = tw > 0 ? tw + 2 * c.gap : 0;
    const double he = halfExtent(uu, rotText, tw, c.th);

    bool arrowsIn = false, textIn = true;
    Vec2 center{};
    double lineEnd = 0;
    std::vector<Vec2> leader;
    if (d.textPosUser) {
        arrowsIn = len >= 2 * c.arrow;
        center = d.textPos;
        double tc = dot(center - A, uu);
        textIn = tc > 0 && tc < len;
        lineEnd = (arrowsIn ? len : len + stub) + c.dext;
        if (!textIn && tw > 0) {
            Vec2 E = tc <= 0 ? A - uu * (arrowsIn ? 0 : stub) : B + uu * (arrowsIn ? 0 : stub);
            Vec2 v = center - E;
            double dl = length(v);
            if (dl > tol::kLinear) {
                Vec2 q = v / dl;
                double reach = halfExtent(q, rotText, tw, c.th) + c.gap;
                if (dl > reach)
                    leader = {E, center - q * reach};
            }
        }
    } else {
        if (len >= textW + 2 * c.arrow) {
            arrowsIn = true;
            textIn = true;
        } else if (tw > 0 && len >= textW) {
            arrowsIn = false;
            textIn = true;
        } else if (len >= 2 * c.arrow) {
            arrowsIn = true;
            textIn = false;
        } else {
            arrowsIn = false;
            textIn = false;
        }
        if (textIn) {
            center = A + uu * (len / 2) + lift.offset;
            lineEnd = (arrowsIn ? len : len + stub) + c.dext;
        } else {
            double lend = arrowsIn ? len : len + stub;
            double near = lend + c.gap;
            center = A + uu * (near + he) + lift.offset;
            lineEnd = lift.centered ? lend : near + 2 * he;
        }
    }
    const double lineStart = (arrowsIn ? 0 : -stub) - (arrowsIn ? c.dext : 0);

    // Dimension line, broken around text that sits on it.
    double b0 = 0, b1 = 0;
    bool brk = false;
    if (tw > 0 && textIn) {
        double lateral = std::abs(dot(center - A, perp(uu)));
        if (lateral < c.th / 2 + c.gap) {
            double tc = dot(center - A, uu);
            double hb = he + c.gap;
            b0 = tc - hb;
            b1 = tc + hb;
            brk = true;
        }
    }
    auto seg = [&](double s0, double s1) {
        if (s1 - s0 > c.tol)
            addSeg(L, A + uu * s0, A + uu * s1, Role::DimLine, c.tol);
    };
    if (brk) {
        seg(lineStart, std::min(lineEnd, b0));
        seg(std::max(lineStart, b1), lineEnd);
    } else {
        seg(lineStart, lineEnd);
    }
    if (leader.size() == 2)
        addSeg(L, leader[0], leader[1], Role::DimLine, c.tol);

    addArrow(L, c, A, arrowsIn ? uu : -uu);
    addArrow(L, c, B, arrowsIn ? -uu : uu);
    if (tw > 0 || !txt.empty())
        L.text = {txt, center, rotText, c.th, false, false, tw};
}

// ---- radius and diameter -----------------------------------------------------------------------

void layoutRadial(const Ctx& c, const DimensionData& d, const std::string& txt, Layout& L) {
    const bool dia = d.kind == DimKind::Diameter;
    const Vec2 A = d.defPts[0], B = d.defPts[1];  // radius: centre and circle point
    const double len = distance(A, B);
    const Vec2 dir = len > c.tol ? (B - A) / len : Vec2{1, 0};
    const Vec2 O = dia ? lerp(A, B, 0.5) : A;
    const double r = dia ? len / 2 : len;

    const double tw = textWidth(c, txt);
    const double rotText = c.st.textAlign == DimTextAlign::Horizontal ? 0 : readable(angleOf(dir));
    const double he = halfExtent(dir, rotText, tw, c.th);
    const Lift lift = liftFor(c, dir, rotText);

    bool inside = false;
    Vec2 center{};
    if (d.textPosUser) {
        center = d.textPos;
        inside = distance(O, center) < r;
    } else if (dia && len - 2 * c.arrow >= tw + 2 * c.gap) {
        inside = true;
        center = O + lift.offset;
    } else {
        center = B + dir * (c.arrow + c.gap + he);
    }

    // Dimension line: the whole diameter / radius, broken around text lying on it; text outside
    // the circle gets a short leader from the circle point.
    double lateral = std::abs(dot(center - A, perp(dir)));
    if (inside && tw > 0 && lateral < c.th / 2 + c.gap) {
        double tc = dot(center - A, dir);
        double hb = he + c.gap;
        if (tc - hb > c.tol)
            addSeg(L, A, A + dir * (tc - hb), Role::DimLine, c.tol);
        if (len - (tc + hb) > c.tol)
            addSeg(L, A + dir * (tc + hb), B, Role::DimLine, c.tol);
    } else {
        addSeg(L, A, B, Role::DimLine, c.tol);
    }
    if (!inside && tw > 0) {
        Vec2 v = center - B;
        double dl = length(v);
        if (dl > tol::kLinear) {
            Vec2 q = v / dl;
            double reach = halfExtent(q, rotText, tw, c.th) + c.gap;
            if (dl > reach)
                addSeg(L, B, center - q * reach, Role::DimLine, c.tol);
        }
    }
    addArrow(L, c, B, -dir);
    if (dia)
        addArrow(L, c, A, dir);
    if (!txt.empty())
        L.text = {txt, center, rotText, c.th, false, false, tw};
}

// ---- angular -----------------------------------------------------------------------------------

void layoutAngular(const Ctx& c, const DimensionData& d, const std::string& txt, Layout& L) {
    AngularGeometry g = angularGeometry(d);
    if (!g.ok)
        return;
    const Vec2 V = g.vertex;
    const double r = g.radius;
    const double a1 = g.startAngle, a2 = g.startAngle + g.sweep;

    // Extension lines: from the end of each picked line to just past the arc.
    const Vec2 dirs[2] = {g.dir1, g.dir2};
    // dir1 / dir2 are the sector sides in start / end order.
    for (int i = 0; i < 2; ++i) {
        double start = 0;
        if (d.kind == DimKind::Angular3Point) {
            // the sides are the two picked points in either order
            for (Vec2 p : {d.defPts[0], d.defPts[1]}) {
                Vec2 v = p - V;
                if (dot(v, dirs[i]) > 0 && std::abs(cross(v, dirs[i])) < 1e-6 * (1 + length(v)))
                    start = std::max(start, length(v));
            }
        } else {
            for (Vec2 p : {d.defPts[0], d.defPts[1], d.defPts[2], d.defPts[3]}) {
                Vec2 v = p - V;
                if (std::abs(cross(v, dirs[i])) < 1e-6 * (1 + length(v)))
                    start = std::max(start, dot(v, dirs[i]));
            }
        }
        if (r > start + c.off)
            addSeg(L, V + dirs[i] * (start + c.off), V + dirs[i] * (r + c.ext), Role::ExtLine,
                   c.tol);
    }

    const double tw = textWidth(c, txt);
    const double arcLen = r * g.sweep;
    const bool arrowsIn = arcLen >= 2 * c.arrow;
    const double stub = 2 * c.arrow;
    const double am = a1 + g.sweep / 2;
    const Vec2 radial = unit(am);
    const double rotText = c.st.textAlign == DimTextAlign::Horizontal ? 0 : readable(am + kPi / 2);

    Vec2 center = V + radial * r;
    bool onArc = true;
    if (d.textPosUser) {
        center = d.textPos;
        onArc = false;
    } else if (d.kind == DimKind::Angular2Line) {
        center = V + radial * r;
    }
    const bool centered = c.st.textPlacement == DimTextPlacement::Centered;
    if (!d.textPosUser && !centered && tw > 0)
        center = center + radial * (c.gap + halfExtent(radial, rotText, tw, c.th));

    const double ea = arrowsIn ? 0 : stub / std::max(r, tol::kLinear);
    double s0 = a1 - ea, s1 = a2 + ea;
    auto arc = [&](double from, double to) {
        if ((to - from) * r > c.tol)
            L.arcs.push_back({V, r, normalizeAngle(from), normalizeAngle(to), Role::DimLine});
    };
    if (onArc && centered && tw > 0) {
        double hb = (halfExtent(perp(radial), rotText, tw, c.th) + c.gap) / std::max(r, 1e-9);
        arc(s0, std::min(s1, am - hb));
        arc(std::max(s0, am + hb), s1);
    } else {
        arc(s0, s1);
    }

    Vec2 t1 = V + unit(a1) * r, t2 = V + unit(a2) * r;
    Vec2 tan1 = perp(unit(a1)), tan2 = -perp(unit(a2));  // pointing into the sector
    addArrow(L, c, t1, arrowsIn ? tan1 : -tan1);
    addArrow(L, c, t2, arrowsIn ? tan2 : -tan2);
    if (!txt.empty())
        L.text = {txt, center, rotText, c.th, false, false, tw};
}

// ---- arc length --------------------------------------------------------------------------------

void layoutArcLength(const Ctx& c, const DimensionData& d, const std::string& txt, Layout& L) {
    ArcSpan g = arcSpan(d);
    if (!g.ok)
        return;
    const Vec2 V = g.center;
    const double r = g.dimRadius;
    const double a1 = g.startAngle, a2 = g.startAngle + g.sweep;

    // Extension lines run along the radii from just off the measured arc to just past the
    // dimension arc.
    const double pr = g.arcRadius;
    for (double a : {a1, a2})
        if (r > pr + c.off)
            addSeg(L, V + unit(a) * (pr + c.off), V + unit(a) * (r + c.ext), Role::ExtLine, c.tol);

    const double tw = textWidth(c, txt);
    const double arcLen = r * g.sweep;
    const bool arrowsIn = arcLen >= 2 * c.arrow;
    const double stub = 2 * c.arrow;
    const double am = a1 + g.sweep / 2;
    const Vec2 radial = unit(am);
    const double rotText = c.st.textAlign == DimTextAlign::Horizontal ? 0 : readable(am + kPi / 2);

    Vec2 center = V + radial * r;
    bool onArc = true;
    if (d.textPosUser) {
        center = d.textPos;
        onArc = false;
    }
    const bool centered = c.st.textPlacement == DimTextPlacement::Centered;
    if (!d.textPosUser && !centered && tw > 0)
        center = center + radial * (c.gap + halfExtent(radial, rotText, tw, c.th));

    const double ea = arrowsIn ? 0 : stub / std::max(r, tol::kLinear);
    double s0 = a1 - ea, s1 = a2 + ea;
    auto arc = [&](double from, double to) {
        if ((to - from) * r > c.tol)
            L.arcs.push_back({V, r, normalizeAngle(from), normalizeAngle(to), Role::DimLine});
    };
    if (onArc && centered && tw > 0) {
        double hb = (halfExtent(perp(radial), rotText, tw, c.th) + c.gap) / std::max(r, 1e-9);
        arc(s0, std::min(s1, am - hb));
        arc(std::max(s0, am + hb), s1);
    } else {
        arc(s0, s1);
    }

    Vec2 t1 = V + unit(a1) * r, t2 = V + unit(a2) * r;
    Vec2 tan1 = perp(unit(a1)), tan2 = -perp(unit(a2));  // pointing into the sector
    addArrow(L, c, t1, arrowsIn ? tan1 : -tan1);
    addArrow(L, c, t2, arrowsIn ? tan2 : -tan2);
    if (!txt.empty())
        L.text = {txt, center, rotText, c.th, false, false, tw};
}

// ---- ordinate ----------------------------------------------------------------------------------

void layoutOrdinate(const Ctx& c, const DimensionData& d, const std::string& txt, Layout& L) {
    const Vec2 P = d.defPts[0], E = d.defPts[1];
    const bool xDatum = d.rotation < kPi / 4;  // measures X: the leader runs along Y
    const Vec2 along = xDatum ? Vec2{0, 1} : Vec2{1, 0};
    const Vec2 across = perp(along);
    const double len = dot(E - P, along);
    const double dir = len >= 0 ? 1.0 : -1.0;
    const double start = c.off;
    if (std::abs(len) <= start)
        return;
    // Short run from the feature, a jog across to the leader's line, then on to the end.
    const double jog = start + (std::abs(len) - start) / 2;
    Layout::Line line;
    line.pts.push_back(P + along * (dir * start));
    const double sideways = dot(E - P, across);
    if (std::abs(sideways) > c.tol) {
        line.pts.push_back(P + along * (dir * jog));
        line.pts.push_back(P + along * (dir * jog) + across * sideways);
    }
    line.pts.push_back(E);
    line.role = Role::DimLine;
    L.lines.push_back(std::move(line));
    if (txt.empty())
        return;
    const double tw = textWidth(c, txt);
    const double rot = xDatum && c.st.textAlign != DimTextAlign::Horizontal ? kPi / 2 : 0;
    Vec2 center = E + along * (dir * (c.gap + halfExtent(along, rot, tw, c.th)));
    if (d.textPosUser)
        center = d.textPos;
    L.text = {txt, center, rot, c.th, false, false, tw};
}

}  // namespace

ArcSpan arcSpan(const DimensionData& d) {
    ArcSpan g;
    const auto& p = d.defPts;
    const Vec2 v1 = p[1] - p[0], v2 = p[2] - p[0];
    g.arcRadius = length(v1);
    g.dimRadius = distance(p[3], p[0]);
    if (g.arcRadius < tol::kLinear)
        return g;
    if (g.dimRadius < g.arcRadius)
        g.dimRadius = g.arcRadius;
    g.center = p[0];
    g.startAngle = std::atan2(v1.y, v1.x);
    const double end = std::atan2(v2.y, v2.x);
    g.sweep = normalizeAngle(end - g.startAngle);
    if (g.sweep < 1e-9)
        g.sweep = kTwoPi;
    g.ok = true;
    return g;
}

AngularGeometry angularGeometry(const DimensionData& d) {
    AngularGeometry g;
    const auto& p = d.defPts;
    Vec2 e1, e2, w{};
    bool fourWay = false;
    if (d.kind == DimKind::Angular2Line) {
        e1 = normalized(p[1] - p[0]);
        e2 = normalized(p[3] - p[2]);
        double den = cross(e1, e2);
        if (tol::isZeroLength(e1) || tol::isZeroLength(e2) || std::abs(den) < 1e-9)
            return g;
        double t = cross(p[2] - p[0], e2) / den;
        g.vertex = p[0] + e1 * t;
        w = d.textPos - g.vertex;
        fourWay = true;
    } else if (d.kind == DimKind::Angular3Point) {
        g.vertex = p[2];
        e1 = normalized(p[0] - p[2]);
        e2 = normalized(p[1] - p[2]);
        if (tol::isZeroLength(e1) || tol::isZeroLength(e2))
            return g;
        w = p[3] - g.vertex;
    } else {
        return g;
    }
    // Direction the arc is on; without one, the middle of the first sector.
    double maxd = 0;
    for (const Vec2& q : p)
        maxd = std::max(maxd, distance(q, g.vertex));
    Vec2 wn = normalized(w);
    bool haveW = !tol::isZeroLength(wn);
    g.radius = haveW ? length(w) : maxd + 1;
    if (!(g.radius > tol::kLinear))
        g.radius = maxd + 1;

    auto sector = [](Vec2 d1, Vec2 d2, double& start, double& sweep) {
        double a = angleOf(d1), b = angleOf(d2);
        double sw = normalizeAngle(b - a);
        if (sw < kPi) {
            start = a;
            sweep = sw;
        } else {
            start = b;
            sweep = kTwoPi - sw;
        }
    };
    auto contains = [](double start, double sweep, Vec2 dir) {
        return normalizeAngle(angleOf(dir) - start) <= sweep + 1e-9;
    };
    struct Cand {
        Vec2 d1, d2;
    };
    std::vector<Cand> cands;
    if (fourWay) {
        for (double s1 : {1.0, -1.0})
            for (double s2 : {1.0, -1.0})
                cands.push_back({e1 * s1, e2 * s2});
    } else {
        cands.push_back({e1, e2});
    }
    bool found = false;
    for (const Cand& cd : cands) {
        double start, sweep;
        sector(cd.d1, cd.d2, start, sweep);
        if (haveW && !contains(start, sweep, wn))
            continue;
        g.startAngle = start;
        g.sweep = sweep;
        g.dir1 = unit(start);
        g.dir2 = unit(start + sweep);
        found = true;
        break;
    }
    if (!found && !fourWay) {
        // Three-point dimension whose arc point lies in the reflex sector.
        double a = angleOf(e1), b = angleOf(e2);
        double sw = normalizeAngle(b - a);
        double start = a, sweep = sw;
        if (haveW && !contains(start, sweep, wn)) {
            start = b;
            sweep = kTwoPi - sw;
        }
        g.startAngle = start;
        g.sweep = sweep;
        g.dir1 = unit(start);
        g.dir2 = unit(start + sweep);
        found = true;
    }
    if (!found) {
        double start, sweep;
        sector(cands[0].d1, cands[0].d2, start, sweep);
        g.startAngle = start;
        g.sweep = sweep;
        g.dir1 = unit(start);
        g.dir2 = unit(start + sweep);
    }
    g.ok = g.sweep > 1e-9;
    return g;
}

namespace {
// The style with its overall scale multiplied by the annotation scale.
DimStyle scaledStyle(const DimStyle& base, double annoScale) {
    DimStyle s = base;
    if (annoScale > 0 && annoScale != 1.0)
        s.overallScale = (s.overallScale > 0 ? s.overallScale : 1.0) * annoScale;
    return s;
}
}  // namespace

Layout layoutDimension(const Document& doc, const DimensionData& d, double tolerance,
                       double annoScale) {
    Layout L;
    const DimStyle st = scaledStyle(styleOf(doc, d.style), annoScale);
    Ctx c(doc, st, tolerance > 0 ? tolerance : 0.01);
    DimensionData shown = d;
    if (text::hasFields(shown.textOverride))
        shown.textOverride = text::expandFields(doc, shown.textOverride);
    const std::string txt = dimensionText(st, shown);
    switch (d.kind) {
        case DimKind::Linear:
        case DimKind::Aligned:
            layoutLinear(c, d, txt, L);
            break;
        case DimKind::Radius:
        case DimKind::Diameter:
            layoutRadial(c, d, txt, L);
            break;
        case DimKind::Angular2Line:
        case DimKind::Angular3Point:
            layoutAngular(c, d, txt, L);
            break;
        case DimKind::ArcLength:
            layoutArcLength(c, d, txt, L);
            break;
        case DimKind::Ordinate:
            layoutOrdinate(c, d, txt, L);
            break;
    }
    return L;
}

Layout layoutLeader(const Document& doc, const LeaderData& d, double tolerance, double annoScale) {
    Layout L;
    if (d.vertices.size() < 2)
        return L;
    const DimStyle st = scaledStyle(styleOf(doc, d.style), annoScale);
    Ctx c(doc, st, tolerance > 0 ? tolerance : 0.01);
    Layout::Line line;
    line.pts = d.vertices;
    L.lines.push_back(std::move(line));
    addArrow(L, c, d.vertices[0], normalized(d.vertices[1] - d.vertices[0]));
    const Vec2 last = d.vertices.back();
    const bool right = d.textPos.x >= last.x;
    if (!d.utf8.empty()) {
        addSeg(L, last, last + Vec2{right ? c.arrow : -c.arrow, 0}, Role::DimLine, c.tol);
        L.text = {d.utf8, d.textPos, 0, c.th, true, !right, 0};
    }
    return L;
}

void refresh(const Document& doc, DimensionData& d) {
    d.measurement = measurement(d);
    // The two-line angular kind keeps the arc position in textPos, so it is never recomputed.
    if (d.textPosUser || d.kind == DimKind::Angular2Line)
        return;
    Layout L = layoutDimension(doc, d);
    if (L.text.valid())
        d.textPos = L.text.center;
}

std::vector<Vec2> arcPoints(const Layout::Arc& a, double tolerance) {
    double sweep = normalizeAngle(a.endAngle - a.startAngle);
    if (sweep < 1e-12)
        sweep = kTwoPi;
    double step = kPi / 2;
    if (a.radius > tolerance)
        step = std::min(step, 2 * std::acos(std::max(0.0, 1 - tolerance / a.radius)));
    int n = std::max(2, static_cast<int>(std::ceil(sweep / step)));
    std::vector<Vec2> pts;
    pts.reserve(static_cast<std::size_t>(n) + 1);
    for (int i = 0; i <= n; ++i)
        pts.push_back(a.center + unit(a.startAngle + sweep * i / n) * a.radius);
    return pts;
}

}  // namespace tbcad::dim
