#include <algorithm>
#include <cmath>
#include <optional>

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
    double th, arrow, gap, off, ext, dext, fxl;  // style values times the overall scale
    // The width of the text when it holds a stacked tolerance (measured from its parts), for the
    // plain text `stackedPlain`; < 0 when nothing is stacked.
    double stackedWidth = -1;
    std::string stackedPlain{};

    Ctx(const Document& d, const DimStyle& s, double t) : doc(d), st(s), tol(t) {
        const double k = s.overallScale > 0 ? s.overallScale : 1.0;
        tstyle = text::styleFor(d, s.textStyle);
        th = s.textHeight * k;
        arrow = s.arrowSize * k;
        gap = s.textGap * k;
        off = s.extLineOffset * k;
        ext = s.extLineExtension * k;
        dext = s.dimLineExtension * k;
        fxl = s.fixedExtLength * k;
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
    if (c.stackedWidth >= 0 && s == c.stackedPlain)
        return c.stackedWidth;
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
    // DIMTVP moved the text off the line by `offset`: the line breaks round it when this is set.
    std::optional<bool> tvpBreak{};
};
Lift liftFor(const Ctx& c, Vec2 lineDir, double rotText,
             std::optional<DimTextAlign> align = std::nullopt) {
    Lift l;
    const DimTextAlign al = align.value_or(c.st.textAlign);
    l.centered = c.st.textPlacement == DimTextPlacement::Centered ||
                 (al == DimTextAlign::Horizontal && std::abs(lineDir.y) > 0.5);
    if (!l.centered) {
        l.offset = perp(unit(rotText)) *
                   ((c.gap + c.th / 2) * (c.st.textPlacement == DimTextPlacement::Below ? -1 : 1));
    } else if (c.st.textVertPos != 0 &&
               !(al == DimTextAlign::Horizontal && std::abs(lineDir.y) > 0.5)) {
        // DIMTVP, as in AutoCAD: the text moves DIMTVP text heights off the line, which breaks
        // round it only when the move is less than 0.7 of the text height.
        l.offset = perp(unit(rotText)) * (c.st.textVertPos * c.th);
        l.tvpBreak = std::abs(c.st.textVertPos) < 0.7;
        l.centered = *l.tvpBreak;
    }
    return l;
}

// Text that sits on the dimension line (which then breaks around it). Text lifted exactly its own
// clearance above the line is not on it; the margin keeps rounding noise (far from the origin)
// from toggling the break.
bool textOnLine(const Ctx& c, double lateral) {
    return lateral < c.th / 2 + c.gap - 1e-6 * std::max(1.0, c.th + c.gap);
}

// ---- linear and aligned ------------------------------------------------------------------------

void layoutLinear(const Ctx& c, const DimensionData& d, const std::string& txt, Layout& L) {
    const Vec2 p1 = d.defPts[0], p2 = d.defPts[1], P = d.defPts[2];
    Vec2 u = d.kind == DimKind::Linear ? unit(d.rotation) : normalized(p2 - p1);
    if (tol::isZeroLength(u))
        u = {1, 0};
    const Vec2 n = perp(u);
    // Extension lines lean by the oblique angle; they still end on the dimension line.
    const Vec2 e =
        d.oblique == 0 ? n : normalized(n * std::cos(d.oblique) - u * std::sin(d.oblique));
    const double lean = dot(e, n);
    const double s1 = dot(P - p1, n) / lean, s2 = dot(P - p2, n) / lean;
    const Vec2 f1 = p1 + e * s1, f2 = p2 + e * s2;

    const Vec2 pts[2] = {p1, p2};
    const Vec2 feet[2] = {f1, f2};
    const double ss[2] = {s1, s2};
    for (int i = 0; i < 2; ++i) {
        double dist = std::abs(ss[i]);
        // An offset longer than the distance to the dimension line still leaves the part beyond
        // it, as AutoCAD draws it.
        if (dist <= c.tol || dist + c.ext <= c.off ||
            (i == 0 ? c.st.suppressExt1 : c.st.suppressExt2))
            continue;
        double sg = ss[i] >= 0 ? 1 : -1;
        // A fixed length runs from the dimension line back toward the origin.
        const double from = c.st.fixedExtLines ? std::max(c.off, dist - c.fxl) : c.off;
        L.lines.push_back({{pts[i] + e * (sg * from), feet[i] + e * (sg * c.ext)}, Role::ExtLine});
    }

    const bool swap = dot(f1, u) > dot(f2, u);
    const Vec2 A = swap ? f2 : f1, B = swap ? f1 : f2;
    const double len = distance(A, B);
    const Vec2 uu = len > c.tol ? normalized(B - A) : u;

    const double tw = textWidth(c, txt);
    auto rotFor = [&](DimTextAlign al) {
        return d.textRotated                    ? d.textRotation
               : al == DimTextAlign::Horizontal ? 0
                                                : readable(angleOf(uu));
    };
    double rotText = rotFor(c.st.textAlign);
    Lift lift = liftFor(c, uu, rotText);
    const double stub = 2 * c.arrow;
    const double textW = tw > 0 ? tw + 2 * c.gap : 0;
    double he = halfExtent(uu, rotText, tw, c.th);
    // Text beyond the extension lines follows DIMTOH instead of DIMTIH.
    auto textOutside = [&] {
        rotText = rotFor(c.st.textAlignOutside);
        lift = liftFor(c, uu, rotText, c.st.textAlignOutside);
        he = halfExtent(uu, rotText, tw, c.th);
    };

    bool arrowsIn = false, textIn = true;
    // DIMATFIT decides what leaves first when text and arrows do not fit together.
    auto decideFit = [&] {
        const DimFit fit = c.st.fit;
        if (len >= textW + 2 * c.arrow) {
            arrowsIn = true;
            textIn = true;
        } else if (c.st.textInsideForced && tw > 0) {  // DIMTIX
            arrowsIn = false;
            textIn = true;
        } else if (fit == DimFit::BothOutside) {
            arrowsIn = false;
            textIn = false;
        } else if (fit == DimFit::TextFirst) {
            if (len >= 2 * c.arrow) {
                arrowsIn = true;
                textIn = false;
            } else if (tw > 0 && len >= textW) {
                arrowsIn = false;
                textIn = true;
            } else {
                arrowsIn = false;
                textIn = false;
            }
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
    };
    Vec2 center{};
    double lineEnd = 0;
    double textBeforeA = 0;  // how far the dimension line runs before A to text placed there
    std::vector<Vec2> leader;
    Vec2 point{};  // the text point (DXF group 11)
    if (d.textPosUser) {
        center = point = d.textPos;
        double tc = dot(center - A, uu);
        textIn = tc > 0 && tc < len;
        // Arrows stay inside when they fit; DIMTIX text between the extension lines that leaves
        // them no room sends them out.
        arrowsIn = len >= 2 * c.arrow &&
                   !(c.st.textInsideForced && textIn && tw > 0 && len < textW + 2 * c.arrow);
        if (!textIn)
            textOutside();
        // A point on the dimension line is under (or over) the text, which sits lifted from it
        // (DIMTAD), as AutoCAD writes it; a point away from the line is the middle of the text.
        if (c.st.textMove == DimTextMove::WithLine &&
            std::abs(dot(point - A, perp(uu))) < 1e-3 * std::max(c.th, c.tol))
            center += lift.offset;
        lineEnd = (arrowsIn ? len : len + stub) + c.dext;
        const double lateral = std::abs(dot(point - A, perp(uu)));
        if (c.st.textMove == DimTextMove::Leader && tw > 0 && lateral > c.gap + 1.5 * c.th) {
            // Moved away with a leader (DIMTMOVE 1), as AutoCAD draws it: the point is where the
            // text starts; a leader runs from the middle of the dimension line to a landing line
            // under the text (or to the text's edge when the text is centred on its line).
            textOutside();
            const Vec2 t = unit(rotText), up = perp(t);
            const Vec2 mid = A + uu * (len / 2);
            const double side = dot(d.textPos - mid, t) >= 0 ? 1 : -1;
            const double raise = lift.centered ? 0
                                 : c.st.textPlacement == DimTextPlacement::Below
                                     ? -(c.gap + c.th / 2)
                                     : c.gap + c.th / 2;
            center = d.textPos + t * (side * tw / 2);
            const Vec2 l0 = d.textPos - t * (side * c.gap) - up * raise;
            leader = {mid, l0};
            if (raise != 0)
                leader.push_back(d.textPos + t * (side * (tw + c.gap)) - up * raise);
            textIn = false;
        } else if (!textIn && tw > 0 && c.st.textMove != DimTextMove::Free) {
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
        decideFit();
        if (textIn) {
            double along = len / 2;
            // DIMJUST: next to one extension line, when the text fits there with the arrows.
            if (c.st.textJustify != 0 && arrowsIn && len >= textW + 4 * c.arrow) {
                const bool nearA = (c.st.textJustify == 1) != swap;
                const double edge = 2 * c.arrow + textW / 2;
                along = nearA ? edge : len - edge;
            }
            center = A + uu * along + lift.offset;
            lineEnd = (arrowsIn ? len : len + stub) + c.dext;
        } else {
            // Text that does not fit goes beside the second extension line, as in AutoCAD.
            textOutside();
            const double beyond = arrowsIn ? 0 : stub;
            const double near = beyond + c.gap;
            if (swap) {
                center = A - uu * (near + he) + lift.offset;
                lineEnd = len + beyond;
                textBeforeA = lift.centered ? beyond : near + 2 * he;
            } else {
                center = A + uu * (len + near + he) + lift.offset;
                lineEnd = len + (lift.centered ? beyond : near + 2 * he);
            }
        }
    }
    // DIMSOXD: arrows that do not fit are left out (with their stubs).
    const bool noArrows = !arrowsIn && c.st.suppressArrowsOutside;
    const double lineStart = textBeforeA > 0 ? -textBeforeA
                                             : (arrowsIn   ? 0
                                                : noArrows ? 0
                                                           : -stub) -
                                                   (arrowsIn ? c.dext : 0);
    if (noArrows && textIn)
        lineEnd = len;
    // DIMTOFL off: arrows outside leave nothing between the extension lines.
    const bool gapInside = !arrowsIn && !c.st.forceLineInside && !noArrows;

    // Dimension line, broken around text that sits on it.
    double b0 = 0, b1 = 0;
    bool brk = false;
    if (tw > 0 && textIn && !gapInside) {
        double lateral = std::abs(dot(center - A, perp(uu)));
        const bool byTvp =
            lift.tvpBreak && std::abs(lateral - length(lift.offset)) < 1e-6 * std::max(1.0, c.th);
        if (byTvp ? *lift.tvpBreak : textOnLine(c, lateral)) {
            double tc = dot(center - A, uu);
            double hb = he + c.gap;
            b0 = tc - hb;
            b1 = tc + hb;
            brk = true;
        }
    }
    // DIMSD1 / DIMSD2: the half of the line (and its arrow) at that extension line is left out.
    const bool hideA = swap ? c.st.suppressDim2 : c.st.suppressDim1;
    const bool hideB = swap ? c.st.suppressDim1 : c.st.suppressDim2;
    auto seg = [&](double s0, double s1) {
        if (hideA)
            s0 = std::max(s0, len / 2);
        if (hideB)
            s1 = std::min(s1, len / 2);
        if (s1 - s0 > c.tol)
            addSeg(L, A + uu * s0, A + uu * s1, Role::DimLine, c.tol);
    };
    if (gapInside) {
        seg(lineStart, 0);
        seg(len, lineEnd);
    } else if (brk) {
        seg(lineStart, std::min(lineEnd, b0));
        seg(std::max(lineStart, b1), lineEnd);
    } else {
        seg(lineStart, lineEnd);
    }
    for (std::size_t i = 0; i + 1 < leader.size(); ++i)
        addSeg(L, leader[i], leader[i + 1], Role::DimLine, c.tol);

    // Flip Arrow: that arrowhead turns the other way; turned out, it has a stub of line beyond.
    const bool flipA = swap ? d.flipArrow2 : d.flipArrow1;
    const bool flipB = swap ? d.flipArrow1 : d.flipArrow2;
    if (!noArrows) {
        if (!hideA) {
            addArrow(L, c, A, arrowsIn != flipA ? uu : -uu);
            if (arrowsIn && flipA && lineStart > -stub)
                addSeg(L, A - uu * stub, A + uu * std::min(lineStart, 0.0), Role::DimLine, c.tol);
        }
        if (!hideB) {
            addArrow(L, c, B, arrowsIn != flipB ? -uu : uu);
            if (arrowsIn && flipB && lineEnd < len + stub)
                addSeg(L, A + uu * std::max(lineEnd, len), B + uu * stub, Role::DimLine, c.tol);
        }
    }
    if (tw > 0 || !txt.empty()) {
        L.text = {txt, center, rotText, c.th, false, false, tw};
        L.text.point = d.textPosUser ? point : center - lift.offset;
    }
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
    auto rotFor = [&](DimTextAlign al) {
        return al == DimTextAlign::Horizontal ? 0.0 : readable(angleOf(dir));
    };
    double rotText = rotFor(c.st.textAlign);
    double he = halfExtent(dir, rotText, tw, c.th);
    const Lift lift = liftFor(c, dir, rotText);

    bool inside = false;
    Vec2 center{};
    if (d.textPosUser) {
        center = d.textPos;
        inside = distance(O, center) < r;
    } else if (dia && len - 2 * c.arrow >= tw + 2 * c.gap) {
        inside = true;
        center = O + lift.offset;
    }
    if (!inside) {
        // Text beyond the circle is horizontal when DIMTOH asks for it.
        if (c.st.textAlignOutside == DimTextAlign::Horizontal) {
            rotText = 0;
            he = halfExtent(dir, rotText, tw, c.th);
        }
        if (!d.textPosUser)
            center = B + dir * (c.arrow + c.gap + he);
    }

    // Dimension line: the whole diameter / radius, broken around text lying on it; text outside
    // the circle gets a short leader from the circle point.
    double lateral = std::abs(dot(center - A, perp(dir)));
    if (inside && tw > 0 && textOnLine(c, lateral)) {
        double tc = dot(center - A, dir);
        double hb = he + c.gap;
        if (tc - hb > c.tol)
            addSeg(L, A, A + dir * (tc - hb), Role::DimLine, c.tol);
        if (len - (tc + hb) > c.tol)
            addSeg(L, A + dir * (tc + hb), B, Role::DimLine, c.tol);
    } else {
        addSeg(L, A, B, Role::DimLine, c.tol);
    }
    if (!inside && tw > 0 && c.st.textMove != DimTextMove::Free) {
        Vec2 v = center - B;
        double dl = length(v);
        if (dl > tol::kLinear) {
            Vec2 q = v / dl;
            double reach = halfExtent(q, rotText, tw, c.th) + c.gap;
            if (dl > reach)
                addSeg(L, B, center - q * reach, Role::DimLine, c.tol);
        }
    }
    // Flip Arrow turns an arrowhead the other way.
    const bool flipB = dia ? d.flipArrow2 : d.flipArrow1;
    addArrow(L, c, B, flipB ? dir : -dir);
    if (dia)
        addArrow(L, c, A, d.flipArrow1 ? -dir : dir);
    // DIMCEN: a cross at the centre; a negative size also lets the axes run through the circle.
    if (c.st.centerMark != 0) {
        const double k = c.st.overallScale > 0 ? c.st.overallScale : 1.0;
        const double m = std::abs(c.st.centerMark) * k;
        const Vec2 ax{1, 0}, ay{0, 1};
        for (const Vec2 u : {ax, ay}) {
            addSeg(L, O - u * m, O + u * m, Role::DimLine, c.tol);
            if (c.st.centerMark < 0 && r > m) {
                addSeg(L, O + u * (r - m), O + u * (r + m), Role::DimLine, c.tol);
                addSeg(L, O - u * (r + m), O - u * (r - m), Role::DimLine, c.tol);
            }
        }
    }
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
    const bool arrowRoom = arcLen >= 2 * c.arrow;
    // DIMATFIT, as for a linear dimension: text on the arc that fits there, but not together
    // with the arrows, sends the arrows outside (best fit, arrows first, both outside).
    bool textOnArc = tw > 0;
    if (textOnArc && d.textPosUser) {
        const Vec2 v = d.textPos - V;
        const double along = normalizeAngle(angleOf(v) - a1);
        textOnArc = std::abs(length(v) - r) < c.th && along < g.sweep;
    }
    const double textW = tw + 2 * c.gap;
    const bool tight = textOnArc && arcLen < textW + 2 * c.arrow;
    const DimFit fit = c.st.fit;
    const bool arrowsIn =
        arrowRoom && !(tight && (fit == DimFit::ArrowsFirst || fit == DimFit::BothOutside ||
                                 (fit == DimFit::Best && arcLen >= textW)));
    const double stub = 2 * c.arrow;
    const double am = a1 + g.sweep / 2;
    const Vec2 radial = unit(am);
    // DIMTOH: text beyond the extension lines (an arc too short for its arrows, or text moved out
    // past the arc) can be horizontal while the text between them follows the arc.
    // (Moved text sets the arc radius, so "past the arc" is the arc lying beyond the picked lines.)
    const bool outside = !arrowRoom || (d.textPosUser && !L.lines.empty());
    const bool horizontal = c.st.textAlign == DimTextAlign::Horizontal ||
                            (outside && c.st.textAlignOutside == DimTextAlign::Horizontal);
    const double rotText = horizontal ? 0 : readable(am + kPi / 2);

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
    const bool xDatum = d.rotation < kPi / 4;                 // measures X: the leader runs along Y
    const Vec2 ux{std::cos(d.oblique), std::sin(d.oblique)};  // the UCS axes (oblique = X angle)
    const Vec2 along = xDatum ? perp(ux) : ux;
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
    const double rot = xDatum && c.st.textAlign != DimTextAlign::Horizontal
                           ? readable(d.oblique + kPi / 2)
                           : readable(d.oblique);
    Vec2 center = E + along * (dir * (c.gap + halfExtent(along, rot, tw, c.th)));
    if (d.textPosUser)
        center = d.textPos;
    L.text = {txt, center, rot, c.th, false, false, tw};
}

// ---- jogged radius -----------------------------------------------------------------------------

void layoutJogged(const Ctx& c, const DimensionData& d, const std::string& txt, Layout& L) {
    const Vec2 A = d.defPts[1], C = d.defPts[2], J = d.defPts[3];
    const Vec2 u = normalized(A - C);
    if (tol::isZeroLength(u))
        return;
    const Vec2 n = perp(u);
    const double w = 2 * c.arrow;  // length of the jog along the dimension line
    const Vec2 s0 = J - u * (w / 2), s3 = J + u * (w / 2);
    Layout::Line line;
    line.pts = {C, s0, s0 + u * (w * 0.25) + n * (w / 2), s3 - u * (w * 0.25) - n * (w / 2), s3, A};
    line.role = Role::DimLine;
    L.lines.push_back(std::move(line));
    addArrow(L, c, A, -u);
    if (txt.empty())
        return;
    const double tw = textWidth(c, txt);
    const double rot = c.st.textAlign == DimTextAlign::Horizontal ? 0 : readable(angleOf(u));
    Vec2 center = (C + s0) / 2 + perp(unit(rot)) * (c.gap + c.th / 2);
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

// Cuts the gaps of DIMBREAK out of the dimension and extension lines and the dimension arcs. A gap
// applies to a line or arc when both its ends lie on it.
static void applyBreaks(Layout& L, const std::vector<DimBreak>& breaks) {
    constexpr double kOnLine = 0.02;  // mm
    std::vector<Layout::Arc> arcs;
    for (Layout::Arc& arc : L.arcs) {
        double sweep = normalizeAngle(arc.endAngle - arc.startAngle);
        if (sweep < 1e-12)
            sweep = kTwoPi;
        if (arc.role == Role::Text || !(arc.radius > 1e-9)) {
            arcs.push_back(arc);
            continue;
        }
        auto param = [&](Vec2 p, double& u) {
            if (std::abs(distance(p, arc.center) - arc.radius) > kOnLine)
                return false;
            u = normalizeAngle(angleOf(p - arc.center) - arc.startAngle);
            if (u > sweep + kOnLine / arc.radius)  // past the end: the nearer end
                u = (kTwoPi - u < u - sweep) ? 0 : sweep;
            u = std::min(u, sweep);
            return true;
        };
        std::vector<std::pair<double, double>> gaps;
        for (const DimBreak& b : breaks) {
            double u0, u1;
            if (!param(b.a, u0) || !param(b.b, u1))
                continue;
            if (u0 > u1)
                std::swap(u0, u1);
            if (u1 - u0 > 1e-9)
                gaps.emplace_back(u0, u1);
        }
        if (gaps.empty()) {
            arcs.push_back(arc);
            continue;
        }
        std::sort(gaps.begin(), gaps.end());
        double from = 0;
        auto piece = [&](double u0, double u1) {
            arcs.push_back({arc.center, arc.radius, normalizeAngle(arc.startAngle + u0),
                            normalizeAngle(arc.startAngle + u1), arc.role});
        };
        for (const auto& [g0, g1] : gaps) {
            if (g0 > from + 1e-9)
                piece(from, g0);
            from = std::max(from, g1);
        }
        if (from < sweep - 1e-9)
            piece(from, sweep);
    }
    L.arcs = std::move(arcs);
    // Lines, segment by segment (a leader is one line of several points).
    std::vector<Layout::Line> out;
    for (Layout::Line& line : L.lines) {
        if (line.pts.size() < 2 || line.role == Role::Text) {
            out.push_back(std::move(line));
            continue;
        }
        std::vector<Layout::Line> pieces;
        std::vector<Vec2> cur{line.pts[0]};
        auto flush = [&] {
            if (cur.size() >= 2)
                pieces.push_back({cur, line.role});
            cur.clear();
        };
        bool cut = false;
        for (std::size_t i = 0; i + 1 < line.pts.size(); ++i) {
            const Vec2 a = line.pts[i], d = line.pts[i + 1] - a;
            const double len = std::hypot(d.x, d.y);
            std::vector<std::pair<double, double>> gaps;
            if (len > 1e-9) {
                auto param = [&](Vec2 p, double& t) {
                    t = ((p.x - a.x) * d.x + (p.y - a.y) * d.y) / (len * len);
                    const Vec2 q = a + d * t;
                    return std::hypot(p.x - q.x, p.y - q.y) <= kOnLine;
                };
                for (const DimBreak& b : breaks) {
                    double t0, t1;
                    if (!param(b.a, t0) || !param(b.b, t1))
                        continue;
                    if (t0 > t1)
                        std::swap(t0, t1);
                    t0 = std::max(t0, 0.0);
                    t1 = std::min(t1, 1.0);
                    if (t1 - t0 > 1e-9)
                        gaps.emplace_back(t0, t1);
                }
            }
            if (gaps.empty()) {
                cur.push_back(line.pts[i + 1]);
                continue;
            }
            cut = true;
            std::sort(gaps.begin(), gaps.end());
            double from = 0;  // `cur` ends at a + d * from
            for (const auto& [g0, g1] : gaps) {
                if (g1 <= from + 1e-9)
                    continue;
                if (g0 > from + 1e-9)
                    cur.push_back(a + d * g0);
                flush();
                from = g1;
                cur.push_back(a + d * from);
            }
            if (from < 1 - 1e-9)
                cur.push_back(line.pts[i + 1]);
        }
        if (!cut) {
            out.push_back(std::move(line));
            continue;
        }
        flush();
        for (Layout::Line& p : pieces)
            out.push_back(std::move(p));
    }
    L.lines = std::move(out);
}

// DIMJOGLINE: cuts the dimension line open where a jog sits and draws a Z in the gap. A jog is
// placed by its position along the dimension line, so it stays on the line when the line is moved
// (a grip, DIMSPACE, a stretch), as in AutoCAD.
static void applyJogs(Layout& L, const std::vector<Vec2>& jogs, double width) {
    if (!(width > 0))
        return;
    std::vector<Layout::Line> zigzags;
    std::vector<Layout::Line> out;
    for (Layout::Line& line : L.lines) {
        if (line.pts.size() != 2 || line.role != Role::DimLine) {
            out.push_back(std::move(line));
            continue;
        }
        const Vec2 a = line.pts[0], d = line.pts[1] - a;
        const double len = std::hypot(d.x, d.y);
        if (!(len > 2 * width)) {
            out.push_back(std::move(line));
            continue;
        }
        const Vec2 u = d * (1 / len), n = perp(u);
        std::vector<double> at;  // distances along the line, ascending
        for (const Vec2& j : jogs) {
            const double s = (j.x - a.x) * u.x + (j.y - a.y) * u.y;
            if (s > width / 2 && s < len - width / 2)
                at.push_back(s);
        }
        std::sort(at.begin(), at.end());
        double from = 0;
        for (double s : at) {
            if (s - width / 2 < from)
                continue;  // two jogs too close: the second is skipped
            out.push_back({{a + u * from, a + u * (s - width / 2)}, line.role});
            const Vec2 mid = a + u * s;
            Layout::Line z;
            z.role = Role::DimLine;
            z.pts = {mid - u * (width / 2), mid - u * (width / 6) + n * (width / 2),
                     mid + u * (width / 6) - n * (width / 2), mid + u * (width / 2)};
            zigzags.push_back(std::move(z));
            from = s + width / 2;
        }
        out.push_back({{a + u * from, a + d}, line.role});
    }
    for (Layout::Line& z : zigzags)
        out.push_back(std::move(z));
    L.lines = std::move(out);
}

Layout layoutDimension(const Document& doc, const DimensionData& d, double tolerance,
                       double annoScale) {
    Layout L;
    const DimStyle st = scaledStyle(styleOf(doc, d.style), annoScale);
    Ctx c(doc, st, tolerance > 0 ? tolerance : 0.01);
    // A dimension style's text style with a fixed height sets the text's height, as in AutoCAD:
    // DIMTXT and DIMSCALE do not apply to it.
    if (st.textStyle.value != 0 && c.tstyle && c.tstyle->height > 0)
        c.th = c.tstyle->height;
    DimensionData shown = d;
    if (text::hasFields(shown.textOverride))
        shown.textOverride = text::expandFields(doc, shown.textOverride);
    const std::string txt = dimensionText(st, shown);
    const std::optional<StackedText> stacked = stackedDimensionText(st, shown);
    if (stacked) {
        // Each line of the stack is DIMTFAC times the text height; it is as wide as its wider part.
        const double sh = c.th * st.tolHeightFactor;
        c.stackedWidth = textWidth(c, stacked->before) + textWidth(c, stacked->after) +
                         std::max(text::measureWidth(c.tstyle, stacked->up, sh),
                                  text::measureWidth(c.tstyle, stacked->down, sh));
        c.stackedPlain = txt;
    }
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
        case DimKind::Jogged:
            layoutJogged(c, d, txt, L);
            break;
    }
    if (d.kind != DimKind::Linear && d.kind != DimKind::Aligned)
        L.text.point = L.text.center;
    // Text fixed at an angle (the Angle option, DIMEDIT Rotate); linear and aligned dimensions
    // already lay their text out at it.
    if (d.textRotated && d.kind != DimKind::Linear && d.kind != DimKind::Aligned)
        L.text.rotation = d.textRotation;
    if (stacked && L.text.utf8 == txt) {
        L.text.utf8 = stacked->markup;
        L.text.stacked = true;
    }
    if (!d.breaks.empty())
        applyBreaks(L, d.breaks);
    if (!d.jogs.empty())
        applyJogs(L, d.jogs, 2 * c.arrow);
    // A box round the text (DIMGAP below zero): the text plus a gap on every side.
    if (st.textFrame && L.text.valid() && L.text.width > 0) {
        const Vec2 ux = unit(L.text.rotation), uy = perp(ux);
        const double hw = L.text.width / 2 + c.gap, hh = L.text.height / 2 + c.gap;
        const Vec2 m = L.text.center;
        Layout::Line box;
        box.role = Role::DimLine;
        box.pts = {m - ux * hw - uy * hh, m + ux * hw - uy * hh, m + ux * hw + uy * hh,
                   m - ux * hw + uy * hh, m - ux * hw - uy * hh};
        L.lines.push_back(std::move(box));
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
    if (!d.breaks.empty())
        applyBreaks(L, d.breaks);
    return L;
}

Layout layoutMLeader(const Document& doc, const MLeaderData& d, double tolerance,
                     double annoScale) {
    Layout L;
    DimStyle st = scaledStyle(styleOf(doc, d.style), annoScale);
    if (d.textHeight > 0)
        st.textHeight = d.textHeight;
    Ctx c(doc, st, tolerance > 0 ? tolerance : 0.01);
    for (const auto& pts : d.lines) {
        if (pts.size() < 2)
            continue;
        Layout::Line line;
        line.pts = pts;
        L.lines.push_back(std::move(line));
        addArrow(L, c, pts[0], normalized(pts[1] - pts[0]));
    }
    const bool right = d.contentPos.x >= d.landing.x;
    if (d.dogleg)
        addSeg(L, d.landing, d.contentPos, Role::DimLine, c.tol);
    if (d.content == MLeaderData::Content::Text && !d.utf8.empty()) {
        const Vec2 at = d.contentPos + Vec2{right ? c.gap : -c.gap, 0};
        L.text = {d.utf8, at, 0, c.th, true, !right, 0};
    }
    if (!d.breaks.empty())
        applyBreaks(L, d.breaks);
    return L;
}

void refresh(const Document& doc, DimensionData& d) {
    d.measurement = measurement(d);
    // The two-line angular kind keeps the arc position in textPos, so it is never recomputed.
    if (d.textPosUser || d.kind == DimKind::Angular2Line)
        return;
    Layout L = layoutDimension(doc, d);
    if (L.text.valid())
        d.textPos = L.text.point;
}

void placeText(const Document& doc, DimensionData& d, Vec2 at) {
    d.textPos = at;
    d.textPosUser = true;
    if ((d.kind != DimKind::Linear && d.kind != DimKind::Aligned) ||
        styleOf(doc, d.style).textMove != DimTextMove::WithLine)
        return;
    Vec2 u = d.kind == DimKind::Linear ? unit(d.rotation) : normalized(d.defPts[1] - d.defPts[0]);
    if (tol::isZeroLength(u))
        u = {1, 0};
    const Vec2 n = perp(u);
    d.defPts[2] = d.defPts[2] + n * dot(at - d.defPts[2], n);
    // With the line through the point the text sits lifted from it; move the line back by that.
    const Vec2 lifted = layoutDimension(doc, d).text.center;
    d.defPts[2] = d.defPts[2] - n * dot(lifted - at, n);
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
