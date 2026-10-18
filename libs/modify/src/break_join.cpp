#include "tbcad/modify/break_join.h"

#include <algorithm>
#include <cmath>

namespace tbcad::modify {

using geom::CurveRef;

namespace {

Error err(std::string m) {
    return Error{ErrorCode::InvalidArgument, std::move(m)};
}

double fractionOf(const CurveRef& c, double t) {
    if (c.isLine())
        return std::clamp(t, 0.0, 1.0);
    double off = normalizeAngle(t - c.t0());
    if (c.kind() == CurveRef::Kind::Spline)
        return std::clamp((t - c.t0()) / c.sweep(), 0.0, 1.0);
    if (off > c.sweep())
        return (off - c.sweep() < kTwoPi - off) ? 1.0 : 0.0;
    return off / c.sweep();
}

double paramOfFraction(const CurveRef& c, double u) {
    return c.isLine() ? u : c.t0() + u * c.sweep();
}

std::optional<EntityData> subData(const Entity& e, const CurveRef& c, double u0, double u1) {
    if (u1 - u0 <= 1e-12)
        return std::nullopt;
    double ta = paramOfFraction(c, u0), tb = paramOfFraction(c, u1);
    auto sub = c.subcurve(ta, tb);
    if (!sub || sub.value().length() <= tol::kLinear)
        return std::nullopt;
    if (c.kind() == CurveRef::Kind::Spline) {
        auto sp = geom::splineSubcurve(*entityAs<SplineData>(e), ta, tb);
        if (!sp)
            return std::nullopt;
        return EntityData{Indirect<SplineData>(std::move(sp.value()))};
    }
    return dataFromCurve(sub.value());
}

Result<EntityEdit> pieces(const Entity& e, std::vector<EntityData> data) {
    EntityEdit r;
    if (data.empty()) {
        r.eraseSource = true;
        return r;
    }
    r.modified = withData(e, std::move(data[0]));
    for (std::size_t i = 1; i < data.size(); ++i)
        r.added.push_back(withData(e, std::move(data[i])));
    return r;
}

Result<EntityEdit> breakPolyline(const Entity& e, const PolylineData& raw, Vec2 p1,
                                 std::optional<Vec2> p2) {
    PolylineData pl = cleanPolyline(raw);
    auto segs = polylineSegs(pl);
    if (segs.empty())
        return err("The polyline has no length.");
    const double n = static_cast<double>(segs.size());
    double s1 = closestGlobalParam(segs, p1);
    double s2 = p2 ? closestGlobalParam(segs, *p2) : s1;
    if (s1 > s2)
        std::swap(s1, s2);
    if (p2 && s2 - s1 <= 1e-9)
        return err("The two break points are the same.");
    std::vector<std::vector<Seg>> parts;
    if (pl.closed) {
        parts.push_back(sliceCyclic(segs, s2, s1));
    } else if (p2) {
        parts.push_back(sliceSegs(segs, 0, s1));
        parts.push_back(sliceSegs(segs, s2, n));
    } else {
        if (s1 <= 1e-9 || s1 >= n - 1e-9)
            return err("The point is at an end of the polyline.");
        parts.push_back(sliceSegs(segs, 0, s1));
        parts.push_back(sliceSegs(segs, s1, n));
    }
    std::vector<EntityData> data;
    for (const auto& p : parts) {
        if (!p.empty())
            data.push_back(polylineFromSegs(p, false, &pl));
    }
    return pieces(e, std::move(data));
}

Result<EntityEdit> breakCurve(const Entity& e, Vec2 p1, std::optional<Vec2> p2) {
    std::vector<CurveRef> cs;
    if (!geom::curvesOf(e, cs) || cs.size() != 1)
        return err("This kind of object cannot be broken.");
    const CurveRef& c = cs[0];
    const bool periodic =
        !c.isLine() && c.kind() != CurveRef::Kind::Spline && c.sweep() >= kTwoPi - 1e-9;
    if (periodic) {
        if (!p2)
            return err("A circle needs two break points.");
        double a1 = c.paramAt(p1), a2 = c.paramAt(*p2);
        double sweep = normalizeAngle(a1 - a2);
        if (sweep <= 1e-9)
            return err("The two break points are the same.");
        CurveRef rest = c.kind() == CurveRef::Kind::Arc
                            ? CurveRef::arc(c.center(), c.radius(), a2, sweep)
                            : CurveRef::ellipse(c.center(), c.majorAxis(), c.ratio(), a2, sweep);
        std::vector<EntityData> data;
        if (auto d = dataFromCurve(rest))
            data.push_back(*d);
        return pieces(e, std::move(data));
    }
    if (c.isClosed())
        return err("This closed curve cannot be broken.");
    double u1 = fractionOf(c, c.paramAt(p1));
    double u2 = p2 ? fractionOf(c, c.paramAt(*p2)) : u1;
    if (u1 > u2)
        std::swap(u1, u2);
    std::vector<EntityData> data;
    if (p2) {
        if (u2 - u1 <= 1e-9)
            return err("The two break points are the same.");
        if (auto d = subData(e, c, 0.0, u1))
            data.push_back(*d);
        if (auto d = subData(e, c, u2, 1.0))
            data.push_back(*d);
    } else {
        if (u1 <= 1e-9 || u1 >= 1.0 - 1e-9)
            return err("The point is at an end of the object.");
        if (auto d = subData(e, c, 0.0, u1))
            data.push_back(*d);
        if (auto d = subData(e, c, u1, 1.0))
            data.push_back(*d);
    }
    return pieces(e, std::move(data));
}

Result<EntityEdit> breakAny(const Entity& e, Vec2 p1, std::optional<Vec2> p2) {
    switch (kind(e)) {
        case EntityKind::Line:
        case EntityKind::Circle:
        case EntityKind::Arc:
        case EntityKind::Ellipse:
        case EntityKind::Spline:
            return breakCurve(e, p1, p2);
        case EntityKind::Polyline:
            return breakPolyline(e, *entityAs<PolylineData>(e), p1, p2);
        default:
            return err("This kind of object cannot be broken.");
    }
}

}  // namespace

Result<EntityEdit> breakEntity(const Entity& e, Vec2 p1, Vec2 p2) {
    return breakAny(e, p1, p2);
}

Result<EntityEdit> breakAtPoint(const Entity& e, Vec2 p) {
    return breakAny(e, p, std::nullopt);
}

// ---- Join ---------------------------------------------------------------------------------------

namespace {

struct LineItem {
    Vec2 a{}, b{};
    std::vector<std::size_t> origins{};
};
struct ArcItem {
    Vec2 c{};
    double r = 0, start = 0, sweep = 0;
    std::vector<std::size_t> origins{};
};
struct Item {
    std::vector<Seg> segs{};
    std::vector<std::size_t> origins{};
    std::optional<EntityData> simple{};  // line / arc form, when the item is a single curve
    bool used = false;
    Vec2 head() const { return segs.front().p0; }
    Vec2 tail() const { return segs.back().p1; }
};

bool mergeLines(LineItem& A, const LineItem& B, double tolerance, bool bridgeGaps) {
    Vec2 d = A.b - A.a;
    double len = length(d);
    if (len <= 0)
        return false;
    Vec2 dir = d / len;
    auto off = [&](Vec2 p) { return std::abs(cross(dir, p - A.a)); };
    if (off(B.a) > tolerance || off(B.b) > tolerance)
        return false;
    double tb0 = dot(B.a - A.a, dir), tb1 = dot(B.b - A.a, dir);
    double lo = std::min(tb0, tb1), hi = std::max(tb0, tb1);
    if (!bridgeGaps && (lo > len + tolerance || hi < -tolerance))
        return false;
    double nlo = std::min(0.0, lo), nhi = std::max(len, hi);
    A.b = A.a + dir * nhi;
    A.a = A.a + dir * nlo;
    A.origins.insert(A.origins.end(), B.origins.begin(), B.origins.end());
    return true;
}

// Merges B into A when they lie on one circle and overlap or touch.
bool mergeArcs(ArcItem& A, const ArcItem& B, double tolerance, bool bridgeGaps) {
    if (distance(A.c, B.c) > tolerance || std::abs(A.r - B.r) > tolerance)
        return false;
    const double slack = tolerance / std::max(A.r, 1e-12);
    double oB = normalizeAngle(B.start - A.start);
    double oA = normalizeAngle(A.start - B.start);
    if (oB <= A.sweep + slack || kTwoPi - oB <= slack) {
        if (kTwoPi - oB <= slack)
            oB = 0 - (kTwoPi - oB);  // B starts just before A
        double lo = std::min(0.0, oB), hi = std::max(A.sweep, oB + B.sweep);
        A.start = A.start + lo;
        A.sweep = hi - lo;
    } else if (oA <= B.sweep + slack) {
        double hi = std::max(B.sweep, oA + A.sweep);
        A.start = B.start;
        A.sweep = hi;
    } else if (bridgeGaps) {
        // A gap between them: the arc runs over it, the way round that keeps the arc shorter.
        const double aFirst = oB + B.sweep, bFirst = oA + A.sweep;
        if (std::min(aFirst, bFirst) >= kTwoPi - slack)
            return false;
        if (aFirst <= bFirst) {
            A.sweep = aFirst;
        } else {
            A.start = B.start;
            A.sweep = bFirst;
        }
    } else {
        return false;
    }
    A.origins.insert(A.origins.end(), B.origins.begin(), B.origins.end());
    return true;
}

// ---- splines and elliptical arcs ----------------------------------------------------------------

struct EllipseItem {
    ArcItem arc{};  // centre, major radius and the parameter span
    Vec2 major{};
    double ratio = 1;
};

bool sameEllipseShape(const EllipseItem& a, const EllipseItem& b, double tolerance) {
    return samePoint(a.major, b.major, tolerance) && std::abs(a.ratio - b.ratio) <= 1e-9;
}

// A clamped, open spline: the only kind whose control polygons can be chained exactly.
bool chainable(const SplineData& d) {
    const auto p = static_cast<std::size_t>(d.degree);
    const std::size_t n = d.control.size();
    if (d.closed || d.periodic || d.degree < 1 || n < p + 1 || d.knots.size() != n + p + 1)
        return false;
    if (!d.weights.empty() && d.weights.size() != n)
        return false;
    for (std::size_t i = 0; i < p; ++i) {
        if (d.knots[i] != d.knots[0] || d.knots[n + p - i] != d.knots[n + p])
            return false;
    }
    return d.knots[0] < d.knots[n + p];
}

SplineData reversedSpline(SplineData d) {
    std::reverse(d.control.begin(), d.control.end());
    std::reverse(d.weights.begin(), d.weights.end());
    std::reverse(d.fit.begin(), d.fit.end());
    const double lo = d.knots.front(), hi = d.knots.back();
    for (double& k : d.knots)
        k = lo + hi - k;
    std::reverse(d.knots.begin(), d.knots.end());
    return d;
}

// B's control polygon continues A's: the result is one spline with a C0 joint, the same curve.
SplineData chainSplines(const SplineData& a, const SplineData& b) {
    SplineData out;
    out.degree = a.degree;
    const auto p = static_cast<std::size_t>(a.degree);
    const bool rational = !a.weights.empty() || !b.weights.empty();
    out.control = a.control;
    out.control.insert(out.control.end(), b.control.begin() + 1, b.control.end());
    if (rational) {
        std::vector<double> wa =
            a.weights.empty() ? std::vector<double>(a.control.size(), 1.0) : a.weights;
        std::vector<double> wb =
            b.weights.empty() ? std::vector<double>(b.control.size(), 1.0) : b.weights;
        const double scale = wa.back() / wb.front();  // a common factor leaves B unchanged
        out.weights = wa;
        for (std::size_t i = 1; i < wb.size(); ++i)
            out.weights.push_back(wb[i] * scale);
    }
    out.knots.assign(a.knots.begin(), a.knots.end() - static_cast<std::ptrdiff_t>(p + 1));
    const double joint = a.knots.back();
    const double shift = joint - b.knots.front();
    for (std::size_t i = 1; i < b.knots.size(); ++i)
        out.knots.push_back(b.knots[i] + shift);
    return out;
}

Item reversedItem(const Item& it) {
    Item r = it;
    std::reverse(r.segs.begin(), r.segs.end());
    for (Seg& s : r.segs)
        s = segReversed(s);
    return r;
}

}  // namespace

JoinResult joinEntities(const std::vector<const Entity*>& entities, double tolerance,
                        bool bridgeGaps) {
    JoinResult out;
    std::vector<LineItem> lines;
    std::vector<ArcItem> arcs;
    std::vector<Item> items;  // polylines, filled below together with merged lines / arcs
    for (std::size_t i = 0; i < entities.size(); ++i) {
        const Entity* e = entities[i];
        if (!e)
            continue;
        if (const auto* l = entityAs<LineData>(*e)) {
            if (!tol::isZeroLength(l->b - l->a))
                lines.push_back({l->a, l->b, {i}});
        } else if (const auto* a = entityAs<ArcData>(*e)) {
            double sweep = normalizeAngle(a->endAngle - a->startAngle);
            arcs.push_back(
                {a->center, a->radius, a->startAngle, sweep < 1e-12 ? kTwoPi : sweep, {i}});
        } else if (const auto* p = entityAs<PolylineData>(*e)) {
            PolylineData pl = cleanPolyline(*p);
            if (!pl.closed && pl.v.size() >= 2) {
                Item it;
                it.segs = polylineSegs(pl);
                it.origins = {i};
                items.push_back(std::move(it));
            }
        }
    }
    auto firstProps = [&](const std::vector<std::size_t>& origins) -> const Entity& {
        return *entities[*std::min_element(origins.begin(), origins.end())];
    };

    // Collinear lines and arcs of one circle fuse first.
    for (bool changed = true; changed;) {
        changed = false;
        for (std::size_t i = 0; i < lines.size() && !changed; ++i) {
            for (std::size_t j = i + 1; j < lines.size() && !changed; ++j) {
                if (mergeLines(lines[i], lines[j], tolerance, bridgeGaps)) {
                    lines.erase(lines.begin() + static_cast<std::ptrdiff_t>(j));
                    changed = true;
                }
            }
        }
    }
    for (bool changed = true; changed;) {
        changed = false;
        for (std::size_t i = 0; i < arcs.size() && !changed; ++i) {
            for (std::size_t j = i + 1; j < arcs.size() && !changed; ++j) {
                if (mergeArcs(arcs[i], arcs[j], tolerance, bridgeGaps)) {
                    arcs.erase(arcs.begin() + static_cast<std::ptrdiff_t>(j));
                    changed = true;
                }
            }
        }
    }
    auto consume = [&](const std::vector<std::size_t>& origins) {
        out.consumed.insert(out.consumed.end(), origins.begin(), origins.end());
    };
    for (const LineItem& l : lines) {
        Item it;
        it.segs = {Seg{l.a, l.b, 0}};
        it.origins = l.origins;
        it.simple = LineData{l.a, l.b};
        items.push_back(std::move(it));
    }
    for (const ArcItem& a : arcs) {
        if (a.sweep >= kTwoPi - 1e-9) {
            out.created.push_back(withData(firstProps(a.origins), CircleData{a.c, a.r}));
            consume(a.origins);
            continue;
        }
        Item it;
        Vec2 p0 = a.c + Vec2{std::cos(a.start), std::sin(a.start)} * a.r;
        Vec2 p1 = a.c + Vec2{std::cos(a.start + a.sweep), std::sin(a.start + a.sweep)} * a.r;
        it.segs = {Seg{p0, p1, std::tan(a.sweep / 4.0)}};
        it.origins = a.origins;
        ArcData ad;
        ad.center = a.c;
        ad.radius = a.r;
        ad.startAngle = normalizeAngle(a.start);
        ad.endAngle = normalizeAngle(a.start + a.sweep);
        it.simple = ad;
        items.push_back(std::move(it));
    }

    // Chains of touching items become polylines.
    for (std::size_t i = 0; i < items.size(); ++i) {
        if (items[i].used)
            continue;
        items[i].used = true;
        std::vector<Seg> chain = items[i].segs;
        std::vector<std::size_t> origins = items[i].origins;
        std::size_t count = 1;
        for (bool grew = true; grew;) {
            grew = false;
            for (Item& it : items) {
                if (it.used)
                    continue;
                Vec2 tail = chain.back().p1, head = chain.front().p0;
                auto append = [&](const Item& x) {
                    chain.insert(chain.end(), x.segs.begin(), x.segs.end());
                };
                if (samePoint(tail, it.head(), tolerance)) {
                    append(it);
                } else if (samePoint(tail, it.tail(), tolerance)) {
                    append(reversedItem(it));
                } else if (samePoint(head, it.tail(), tolerance)) {
                    chain.insert(chain.begin(), it.segs.begin(), it.segs.end());
                } else if (samePoint(head, it.head(), tolerance)) {
                    Item r = reversedItem(it);
                    chain.insert(chain.begin(), r.segs.begin(), r.segs.end());
                } else {
                    continue;
                }
                it.used = true;
                origins.insert(origins.end(), it.origins.begin(), it.origins.end());
                ++count;
                grew = true;
                break;
            }
        }
        if (count == 1) {
            if (items[i].origins.size() > 1 && items[i].simple) {
                out.created.push_back(withData(firstProps(origins), *items[i].simple));
                consume(origins);
            }
            continue;
        }
        bool closed = count >= 2 && samePoint(chain.back().p1, chain.front().p0, tolerance);
        out.created.push_back(withData(firstProps(origins), polylineFromSegs(chain, closed)));
        consume(origins);
    }

    // Elliptical arcs of one ellipse fuse like circular arcs; a full turn becomes an ellipse.
    {
        std::vector<EllipseItem> els;
        for (std::size_t i = 0; i < entities.size(); ++i) {
            const Entity* e = entities[i];
            const auto* d = e ? entityAs<EllipseData>(*e) : nullptr;
            if (!d)
                continue;
            double sweep = normalizeAngle(d->endParam - d->startParam);
            if (std::abs(d->endParam - d->startParam) >= kTwoPi - 1e-9)
                continue;
            els.push_back({{d->center,
                            length(d->majorAxis),
                            d->startParam,
                            sweep < 1e-12 ? kTwoPi : sweep,
                            {i}},
                           d->majorAxis,
                           d->ratio});
        }
        for (bool changed = true; changed;) {
            changed = false;
            for (std::size_t i = 0; i < els.size() && !changed; ++i) {
                for (std::size_t j = i + 1; j < els.size() && !changed; ++j) {
                    if (sameEllipseShape(els[i], els[j], tolerance) &&
                        mergeArcs(els[i].arc, els[j].arc, tolerance, bridgeGaps)) {
                        els.erase(els.begin() + static_cast<std::ptrdiff_t>(j));
                        changed = true;
                    }
                }
            }
        }
        for (const EllipseItem& el : els) {
            if (el.arc.origins.size() < 2)
                continue;
            EllipseData d;
            d.center = el.arc.c;
            d.majorAxis = el.major;
            d.ratio = el.ratio;
            if (el.arc.sweep >= kTwoPi - 1e-9) {
                d.startParam = 0;
                d.endParam = kTwoPi;
            } else {
                d.startParam = normalizeAngle(el.arc.start);
                d.endParam = normalizeAngle(el.arc.start + el.arc.sweep);
            }
            out.created.push_back(withData(firstProps(el.arc.origins), d));
            consume(el.arc.origins);
        }
    }

    // Splines whose ends touch become one spline (exact, with a C0 joint).
    {
        struct SplineItem {
            SplineData d{};
            std::vector<std::size_t> origins{};
            bool used = false;
        };
        std::vector<SplineItem> sps;
        for (std::size_t i = 0; i < entities.size(); ++i) {
            const Entity* e = entities[i];
            const auto* d = e ? entityAs<SplineData>(*e) : nullptr;
            if (d && chainable(*d))
                sps.push_back({*d, {i}, false});
        }
        for (std::size_t i = 0; i < sps.size(); ++i) {
            if (sps[i].used)
                continue;
            sps[i].used = true;
            SplineData chain = sps[i].d;
            chain.fit.clear();
            std::vector<std::size_t> origins = sps[i].origins;
            for (bool grew = true; grew;) {
                grew = false;
                for (SplineItem& it : sps) {
                    if (it.used || it.d.degree != chain.degree)
                        continue;
                    const Vec2 tail = chain.control.back(), head = chain.control.front();
                    if (samePoint(tail, it.d.control.front(), tolerance)) {
                        chain = chainSplines(chain, it.d);
                    } else if (samePoint(tail, it.d.control.back(), tolerance)) {
                        chain = chainSplines(chain, reversedSpline(it.d));
                    } else if (samePoint(head, it.d.control.back(), tolerance)) {
                        chain = chainSplines(it.d, chain);
                    } else if (samePoint(head, it.d.control.front(), tolerance)) {
                        chain = chainSplines(reversedSpline(it.d), chain);
                    } else {
                        continue;
                    }
                    it.used = true;
                    origins.insert(origins.end(), it.origins.begin(), it.origins.end());
                    grew = true;
                    break;
                }
            }
            if (origins.size() < 2)
                continue;
            out.created.push_back(
                withData(firstProps(origins), EntityData{Indirect<SplineData>(std::move(chain))}));
            consume(origins);
        }
    }
    std::sort(out.consumed.begin(), out.consumed.end());
    out.consumed.erase(std::unique(out.consumed.begin(), out.consumed.end()), out.consumed.end());
    return out;
}

}  // namespace tbcad::modify
