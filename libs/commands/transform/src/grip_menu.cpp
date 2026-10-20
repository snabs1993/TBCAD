#include "tbcad/transform/grip_menu.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>

#include "tbcad/geom/geom.h"
#include "tbcad/modify/modify.h"

namespace tbcad::transform {

namespace {

constexpr GripAction kAll[] = {
    GripAction::Stretch,         GripAction::AddVertex,      GripAction::RemoveVertex,
    GripAction::ConvertToArc,    GripAction::ConvertToLine,  GripAction::Lengthen,
    GripAction::Radius,          GripAction::ResetText,      GripAction::Continue,
    GripAction::Baseline,        GripAction::FlipArrow,      GripAction::MoveWithLine,
    GripAction::MoveTextOnly,    GripAction::MoveWithLeader, GripAction::AboveLine,
    GripAction::CenterVertically};

bool iequals(const std::string& a, const char* b) {
    std::size_t n = 0;
    while (b[n])
        ++n;
    if (a.size() != n)
        return false;
    for (std::size_t i = 0; i < n; ++i)
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i])))
            return false;
    return true;
}

Error fail(const char* text) {
    return makeError(ErrorCode::InvalidArgument, text);
}

// Splits the arc segment a -> b (bulge) at the point of the arc nearest to `p`; the two bulges.
// Empty when the segment is straight or `p` lies outside the arc.
struct Split {
    Vec2 at{};
    double first = 0, second = 0;
};
std::optional<Split> splitArcSegment(Vec2 a, Vec2 b, double bulge, Vec2 p) {
    auto arc = geom::bulgeArc(a, b, bulge);
    if (!arc)
        return std::nullopt;
    const Vec2 v = p - arc->center;
    if (tol::isZeroLength(v))
        return std::nullopt;
    // Angle travelled from the start of the arc, in the direction the polyline runs.
    const double total = arc->sweep;
    const double aStart =
        arc->reversed ? normalizeAngle(arc->startAngle + arc->sweep) : arc->startAngle;
    double t = std::atan2(v.y, v.x) - aStart;
    t = arc->reversed ? normalizeAngle(-t) : normalizeAngle(t);
    if (t <= 1e-9 || t >= total - 1e-9)
        return std::nullopt;
    const double sign = bulge < 0 ? -1.0 : 1.0;
    Split s;
    const double ang = aStart + sign * t;
    s.at = arc->center + Vec2{std::cos(ang), std::sin(ang)} * arc->radius;
    s.first = sign * std::tan(t / 4);
    s.second = sign * std::tan((total - t) / 4);
    return s;
}

}  // namespace

const char* gripActionLabel(GripAction a) {
    switch (a) {
        case GripAction::Stretch:
            return "Stretch";
        case GripAction::AddVertex:
            return "Add vertex";
        case GripAction::RemoveVertex:
            return "Remove vertex";
        case GripAction::ConvertToArc:
            return "Convert to arc";
        case GripAction::ConvertToLine:
            return "Convert to line";
        case GripAction::Lengthen:
            return "Lengthen";
        case GripAction::Radius:
            return "Radius";
        case GripAction::ResetText:
            return "Reset text position";
        case GripAction::Continue:
            return "Continue dimension";
        case GripAction::Baseline:
            return "Baseline dimension";
        case GripAction::FlipArrow:
            return "Flip arrow";
        case GripAction::MoveWithLine:
            return "Move with dim line";
        case GripAction::MoveTextOnly:
            return "Move text only";
        case GripAction::MoveWithLeader:
            return "Move with leader";
        case GripAction::AboveLine:
            return "Above dim line";
        case GripAction::CenterVertically:
            return "Center vertically";
    }
    return "";
}

const char* gripActionKeyword(GripAction a) {
    switch (a) {
        case GripAction::Stretch:
            return "Stretch";
        case GripAction::AddVertex:
            return "AddVertex";
        case GripAction::RemoveVertex:
            return "RemoveVertex";
        case GripAction::ConvertToArc:
            return "ConvertToArc";
        case GripAction::ConvertToLine:
            return "ConvertToLine";
        case GripAction::Lengthen:
            return "Lengthen";
        case GripAction::Radius:
            return "Radius";
        case GripAction::ResetText:
            return "ResetText";
        case GripAction::Continue:
            return "Continue";
        case GripAction::Baseline:
            return "Baseline";
        case GripAction::FlipArrow:
            return "FlipArrow";
        case GripAction::MoveWithLine:
            return "MoveWithLine";
        case GripAction::MoveTextOnly:
            return "MoveTextOnly";
        case GripAction::MoveWithLeader:
            return "MoveWithLeader";
        case GripAction::AboveLine:
            return "AboveLine";
        case GripAction::CenterVertically:
            return "CenterVertically";
    }
    return "";
}

bool gripActionFromKeyword(const std::string& word, GripAction& out) {
    for (GripAction a : kAll)
        if (iequals(word, gripActionKeyword(a))) {
            out = a;
            return true;
        }
    return false;
}

bool gripActionNeedsPoint(GripAction a) {
    switch (a) {
        case GripAction::RemoveVertex:
        case GripAction::ConvertToLine:
        case GripAction::ResetText:
        case GripAction::Continue:
        case GripAction::Baseline:
        case GripAction::FlipArrow:
        case GripAction::AboveLine:
        case GripAction::CenterVertically:
            return false;
        default:
            return true;
    }
}

bool gripActionStyle(GripAction a, DimStyle& s) {
    switch (a) {
        case GripAction::MoveWithLine:
            s.textMove = DimTextMove::WithLine;
            return true;
        case GripAction::MoveTextOnly:
            s.textMove = DimTextMove::Free;
            return true;
        case GripAction::MoveWithLeader:
            s.textMove = DimTextMove::Leader;
            return true;
        case GripAction::AboveLine:
            s.textPlacement = DimTextPlacement::AboveLine;
            return true;
        case GripAction::CenterVertically:
            s.textPlacement = DimTextPlacement::Centered;
            return true;
        default:
            return false;
    }
}

std::vector<GripAction> gripActions(const Document& doc, const Grip& grip) {
    std::vector<GripAction> out;
    const Entity* e = doc.find(grip.entity);
    if (!e || grip.index < 0 || grip.index >= kDynGripBase)
        return out;
    const int count = static_cast<int>(geom::gripPoints(*e).size());
    if (grip.index >= count)
        return out;
    out.push_back(GripAction::Stretch);
    switch (kind(*e)) {
        case EntityKind::Line:
            if (grip.index < 2)
                out.push_back(GripAction::Lengthen);
            break;
        case EntityKind::Circle:
            if (grip.index >= 1)
                out.push_back(GripAction::Radius);
            break;
        case EntityKind::Arc:
            if (grip.index < 2)
                out.push_back(GripAction::Lengthen);
            else if (grip.index == 2)
                out.push_back(GripAction::Radius);
            break;
        case EntityKind::Polyline: {
            const auto& pl = *entityAs<PolylineData>(*e);
            const int n = static_cast<int>(pl.v.size());
            // The end vertex of an open polyline whose last segment is straight lengthens it.
            if (!pl.closed && n >= 2 &&
                ((grip.index == 0 && pl.v[0].bulge == 0) ||
                 (grip.index == n - 1 && pl.v[static_cast<std::size_t>(n - 2)].bulge == 0)))
                out.push_back(GripAction::Lengthen);
            out.push_back(GripAction::AddVertex);
            if (grip.index < n) {
                if (n > (pl.closed ? 3 : 2))
                    out.push_back(GripAction::RemoveVertex);
            } else {
                const bool arc = pl.v[static_cast<std::size_t>(grip.index - n)].bulge != 0;
                out.push_back(arc ? GripAction::ConvertToLine : GripAction::ConvertToArc);
            }
            break;
        }
        case EntityKind::Leader: {
            const auto& ld = *entityAs<LeaderData>(*e);
            const int n = static_cast<int>(ld.vertices.size());
            if (grip.index < n) {
                out.push_back(GripAction::AddVertex);
                if (n > 2)
                    out.push_back(GripAction::RemoveVertex);
            }
            break;
        }
        case EntityKind::MLine: {
            const auto& ml = *entityAs<MlineData>(*e);
            const int n = static_cast<int>(ml.v.size());
            if (grip.index < n) {
                out.push_back(GripAction::AddVertex);
                if (n > (ml.closed ? 3 : 2))
                    out.push_back(GripAction::RemoveVertex);
            }
            break;
        }
        case EntityKind::MLeader: {
            // The grips of the leader lines come first (all points of line 1, then line 2 ...);
            // after them the landing and the content.
            const auto& ml = *entityAs<MLeaderData>(*e);
            int at = grip.index;
            for (const auto& line : ml.lines) {
                const int n = static_cast<int>(line.size());
                if (at < n) {
                    out.push_back(GripAction::AddVertex);
                    // The last point of a line is where it meets the landing: it stays.
                    if (n > 2 && at < n - 1)
                        out.push_back(GripAction::RemoveVertex);
                    break;
                }
                at -= n;
            }
            break;
        }
        case EntityKind::Spline: {
            // Only a spline defined by fit points can grow or shrink by one point.
            const auto& sp = *entityAs<SplineData>(*e);
            if (!sp.fit.empty()) {
                out.push_back(GripAction::AddVertex);
                if (sp.fit.size() > 2)
                    out.push_back(GripAction::RemoveVertex);
            } else if (!sp.periodic &&
                       sp.control.size() > static_cast<std::size_t>(sp.degree) + 1) {
                // A control vertex can go while the curve keeps degree + 1 of them.
                out.push_back(GripAction::RemoveVertex);
            }
            break;
        }
        case EntityKind::Dimension: {
            const auto& d = *entityAs<DimensionData>(*e);
            if (grip.index == geom::dimTextGrip(d) && d.textPosUser)
                out.push_back(GripAction::ResetText);
            if ((d.kind == DimKind::Linear || d.kind == DimKind::Aligned) &&
                grip.index == geom::dimTextGrip(d)) {
                for (GripAction a : {GripAction::MoveWithLine, GripAction::MoveTextOnly,
                                     GripAction::MoveWithLeader, GripAction::AboveLine,
                                     GripAction::CenterVertically})
                    out.push_back(a);
            }
            // Continue and Baseline from an extension line origin or a dimension line end.
            if ((d.kind == DimKind::Linear || d.kind == DimKind::Aligned) && grip.index < 4) {
                out.push_back(GripAction::Continue);
                out.push_back(GripAction::Baseline);
            }
            // Flip Arrow at a dimension line end (a radius or diameter dimension's curve points).
            const bool radial = d.kind == DimKind::Radius || d.kind == DimKind::Diameter;
            if (((d.kind == DimKind::Linear || d.kind == DimKind::Aligned) &&
                 (grip.index == 2 || grip.index == 3)) ||
                (radial && grip.index < 2 && (d.kind == DimKind::Diameter || grip.index == 1)))
                out.push_back(GripAction::FlipArrow);
            break;
        }
        default:
            break;
    }
    return out;
}

Result<Entity> applyGripAction(const Entity& e, int index, GripAction action, Vec2 target) {
    if (!isFinite(target))
        return fail("the point is not finite");
    if (action == GripAction::Stretch)
        return geom::moveGrip(e, index, target);
    Entity out = e;
    switch (kind(e)) {
        case EntityKind::Line: {
            if (action != GripAction::Lengthen || index > 1)
                break;
            auto& d = *entityAs<LineData>(out);
            const Vec2 fixed = index == 0 ? d.b : d.a;
            const Vec2 moving = index == 0 ? d.a : d.b;
            const Vec2 dir = normalized(moving - fixed);
            const double len = dot(target - fixed, dir);
            if (!(len > tol::kLinear))
                return fail("the line would have no length");
            (index == 0 ? d.a : d.b) = fixed + dir * len;
            return out;
        }
        case EntityKind::Circle: {
            if (action != GripAction::Radius || index < 1 || index > 4)
                break;
            auto& d = *entityAs<CircleData>(out);
            const double r = distance(d.center, target);
            if (tol::isZeroLength(r))
                return fail("the radius would be zero");
            d.radius = r;
            return out;
        }
        case EntityKind::Arc: {
            auto& d = *entityAs<ArcData>(out);
            if (action == GripAction::Radius && index == 2) {
                const double r = distance(d.center, target);
                if (tol::isZeroLength(r))
                    return fail("the radius would be zero");
                d.radius = r;
                return out;
            }
            if (action == GripAction::Lengthen && index < 2) {
                const Vec2 v = target - d.center;
                if (tol::isZeroLength(v))
                    return fail("the point is at the centre");
                const double a = normalizeAngle(std::atan2(v.y, v.x));
                (index == 0 ? d.startAngle : d.endAngle) = a;
                if (tol::nearlyEqualAngle(d.startAngle, d.endAngle))
                    return fail("the arc would have no length");
                return out;
            }
            break;
        }
        case EntityKind::Polyline: {
            auto& pl = *entityAs<PolylineData>(out);
            const int n = static_cast<int>(pl.v.size());
            if (index < 0)
                break;
            const bool onVertex = index < n;
            const int seg = onVertex ? index : index - n;
            if (!onVertex && seg >= geom::segmentCount(pl))
                break;
            switch (action) {
                case GripAction::Lengthen: {
                    // Along the straight end segment, from the vertex before the end.
                    if (!onVertex || pl.closed || n < 2 || (index != 0 && index != n - 1))
                        break;
                    const int other = index == 0 ? 1 : n - 2;
                    if (pl.v[static_cast<std::size_t>(index == 0 ? 0 : n - 2)].bulge != 0)
                        break;
                    const Vec2 fixed = pl.v[static_cast<std::size_t>(other)].p;
                    const Vec2 moving = pl.v[static_cast<std::size_t>(index)].p;
                    const Vec2 dir = normalized(moving - fixed);
                    const double len = dot(target - fixed, dir);
                    if (!(len > tol::kLinear))
                        return fail("the segment would have no length");
                    pl.v[static_cast<std::size_t>(index)].p = fixed + dir * len;
                    return out;
                }
                case GripAction::AddVertex: {
                    const int next = (seg + 1) % n;
                    if (onVertex && !pl.closed && seg == n - 1) {
                        PolyVertex v = pl.v.back();
                        v.p = target;
                        v.bulge = 0;
                        pl.v.push_back(v);
                        return out;
                    }
                    PolyVertex& a = pl.v[static_cast<std::size_t>(seg)];
                    const PolyVertex& b = pl.v[static_cast<std::size_t>(next)];
                    PolyVertex nv = a;
                    nv.p = target;
                    nv.bulge = 0;
                    if (!onVertex && a.bulge != 0) {
                        if (auto s = splitArcSegment(a.p, b.p, a.bulge, target)) {
                            nv.p = s->at;
                            nv.bulge = s->second;
                            a.bulge = s->first;
                        } else {
                            // The point is off the arc: split at the middle of the arc instead.
                            auto arc = geom::bulgeArc(a.p, b.p, a.bulge);
                            if (!arc)
                                return fail("the segment cannot be split");
                            auto s2 = splitArcSegment(
                                a.p, b.p, a.bulge,
                                arc->center + normalized(target - arc->center) * arc->radius);
                            if (!s2)
                                return fail("the point is not on the arc");
                            nv.p = s2->at;
                            nv.bulge = s2->second;
                            a.bulge = s2->first;
                        }
                    } else if (onVertex) {
                        a.bulge = 0;  // the new vertex breaks the curve that left this one
                    }
                    pl.v.insert(pl.v.begin() + seg + 1, nv);
                    return out;
                }
                case GripAction::RemoveVertex: {
                    if (!onVertex || n <= (pl.closed ? 3 : 2))
                        return fail("the polyline needs its remaining vertices");
                    const int prev = (index + n - 1) % n;
                    if (pl.closed || index > 0)
                        pl.v[static_cast<std::size_t>(prev)].bulge = 0;
                    pl.v.erase(pl.v.begin() + index);
                    return out;
                }
                case GripAction::ConvertToArc: {
                    if (onVertex)
                        break;
                    PolyVertex& a = pl.v[static_cast<std::size_t>(seg)];
                    const PolyVertex& b = pl.v[static_cast<std::size_t>((seg + 1) % n)];
                    auto bu = geom::bulgeThrough(a.p, target, b.p);
                    if (!bu)
                        return fail("the three points are on one line");
                    a.bulge = *bu;
                    return out;
                }
                case GripAction::ConvertToLine: {
                    if (onVertex)
                        break;
                    pl.v[static_cast<std::size_t>(seg)].bulge = 0;
                    return out;
                }
                default:
                    break;
            }
            break;
        }
        case EntityKind::Leader: {
            auto& ld = *entityAs<LeaderData>(out);
            const int n = static_cast<int>(ld.vertices.size());
            if (index < 0 || index >= n)
                break;
            if (action == GripAction::AddVertex) {
                ld.vertices.insert(ld.vertices.begin() + index + 1, target);
                return out;
            }
            if (action == GripAction::RemoveVertex) {
                if (n <= 2)
                    return fail("a leader needs two vertices");
                ld.vertices.erase(ld.vertices.begin() + index);
                return out;
            }
            break;
        }
        case EntityKind::MLine: {
            auto& ml = *entityAs<MlineData>(out);
            const int n = static_cast<int>(ml.v.size());
            if (index < 0 || index >= n)
                break;
            // Cuts are kept by segment: the segments a change reshapes lose theirs, those after
            // it move along with the vertex numbers.
            auto remap = [&](std::initializer_list<int> hit, int shift) {
                std::vector<MlineCut> kept;
                for (MlineCut c : ml.cuts) {
                    const int sg = static_cast<int>(c.seg);
                    if (std::find(hit.begin(), hit.end(), sg) != hit.end())
                        continue;
                    if (sg > index)
                        c.seg = static_cast<std::uint32_t>(sg + shift);
                    kept.push_back(c);
                }
                ml.cuts = std::move(kept);
            };
            if (action == GripAction::AddVertex) {
                remap({index}, 1);
                ml.v.insert(ml.v.begin() + index + 1, target);
                return out;
            }
            if (action == GripAction::RemoveVertex) {
                if (n <= (ml.closed ? 3 : 2))
                    return fail("a multiline needs its remaining vertices");
                const int before = ml.closed || index > 0 ? (index + n - 1) % n : index;
                remap({before, index}, -1);
                ml.v.erase(ml.v.begin() + index);
                return out;
            }
            break;
        }
        case EntityKind::MLeader: {
            auto& ml = *entityAs<MLeaderData>(out);
            int at = index;
            for (auto& line : ml.lines) {
                const int n = static_cast<int>(line.size());
                if (at >= n) {
                    at -= n;
                    continue;
                }
                if (action == GripAction::AddVertex) {
                    // After the point, but never past the landing end of the line.
                    line.insert(line.begin() + std::min(at + 1, n - 1), target);
                    return out;
                }
                if (action == GripAction::RemoveVertex) {
                    if (n <= 2 || at >= n - 1)
                        return fail("a leader line needs two points and its landing end");
                    line.erase(line.begin() + at);
                    return out;
                }
                break;
            }
            break;
        }
        case EntityKind::Spline: {
            auto& sp = *entityAs<SplineData>(out);
            if (sp.fit.empty() && action == GripAction::RemoveVertex && index >= 0 &&
                index < static_cast<int>(sp.control.size())) {
                auto r = modify::editSplineCv(e, modify::SplineCvOp::Delete,
                                              sp.control[static_cast<std::size_t>(index)]);
                if (!r)
                    return r.error();
                if (!r.value().modified)
                    return fail("the vertex cannot be removed");
                return *r.value().modified;
            }
            if (sp.fit.empty() || index < 0 || index >= static_cast<int>(sp.fit.size()))
                break;
            if (action == GripAction::AddVertex) {
                // The new fit point goes right after the one under the cursor.
                sp.fit.insert(sp.fit.begin() + index + 1, target);
            } else if (action == GripAction::RemoveVertex) {
                if (sp.fit.size() <= 2)
                    return fail("the spline needs its remaining fit points");
                sp.fit.erase(sp.fit.begin() + index);
            } else {
                break;
            }
            geom::InterpolateOptions o;
            o.degree = sp.degree;
            o.closed = sp.closed;
            auto r = geom::interpolateSpline(sp.fit, o);
            if (!r)
                return r.error();
            sp.degree = r.value().degree;  // fewer points than degree + 1 lower the degree
            sp.knots = r.value().knots;
            sp.control = r.value().control;
            sp.weights.clear();
            return out;
        }
        case EntityKind::Dimension: {
            auto& d = *entityAs<DimensionData>(out);
            if (action == GripAction::FlipArrow) {
                // Linear: line end 2 is at the first extension line, 3 at the second. Radius:
                // its one arrowhead; diameter: the point's own.
                const bool first = d.kind == DimKind::Radius ||
                                   (d.kind == DimKind::Diameter ? index == 0 : index == 2);
                bool& flip = first ? d.flipArrow1 : d.flipArrow2;
                flip = !flip;
                return out;
            }
            if (action == GripAction::ResetText && index == geom::dimTextGrip(d)) {
                d.textPosUser = false;
                return out;
            }
            // The text actions: the style change is the command's (it needs the document); here
            // the text moves to the point, or goes back to its place on the line.
            if (index == geom::dimTextGrip(d)) {
                if (action == GripAction::AboveLine || action == GripAction::CenterVertically) {
                    d.textPosUser = false;
                    return out;
                }
                if (action == GripAction::MoveWithLine || action == GripAction::MoveTextOnly ||
                    action == GripAction::MoveWithLeader)
                    return geom::moveGrip(e, index, target);
            }
            break;
        }
        default:
            break;
    }
    return fail("that action is not available for this grip");
}

std::string gripActionCommand(const Document& doc, const Grip& grip, GripAction action) {
    const auto offered = gripActions(doc, grip);
    if (std::find(offered.begin(), offered.end(), action) == offered.end())
        return {};
    char buf[160];
    if (action == GripAction::Continue || action == GripAction::Baseline) {
        const Entity* e = doc.find(grip.entity);
        const auto* d = e ? entityAs<DimensionData>(*e) : nullptr;
        if (!d)
            return {};
        // The command asks for the base dimension: pick it on its dimension line.
        std::snprintf(buf, sizeof buf, "%s Select %.17g,%.17g",
                      action == GripAction::Continue ? "DIMCONTINUE" : "DIMBASELINE",
                      d->defPts[2].x, d->defPts[2].y);
        return buf;
    }
    std::snprintf(buf, sizeof buf, "GRIPEDIT %.17g,%.17g %s", grip.pos.x, grip.pos.y,
                  gripActionKeyword(action));
    return buf;
}

}  // namespace tbcad::transform
