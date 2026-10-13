#include <algorithm>
#include <array>
#include <cmath>
#include <mutex>
#include <shared_mutex>

#include "tbcad/geom/entity.h"

namespace tbcad::geom {

namespace {

struct RegistryEntry {
    DisplayFn display{};
    BoundsFn bounds{};
};

struct Registry {
    std::shared_mutex mutex;
    std::array<RegistryEntry, static_cast<std::size_t>(EntityKind::Table) + 1> entries{};
};

Registry& registry() {
    static Registry r;
    return r;
}

bool isBuiltIn(EntityKind k) {
    switch (k) {
        case EntityKind::Line:
        case EntityKind::Circle:
        case EntityKind::Arc:
        case EntityKind::Polyline:
        case EntityKind::Point:
        case EntityKind::Ellipse:
        case EntityKind::Spline:
        case EntityKind::Proxy:
            return true;
        default:
            return false;
    }
}

Rgba toRgba(const Color& c) {
    switch (c.kind) {
        case ColorKind::Index:
            return paletteColor(c.indexValue());
        case ColorKind::Rgb:
            return Rgba{c.r, c.g, c.b, 255};
        default:
            return paletteColor(7);
    }
}

void pushRing(std::vector<Vec2>& ring, Vec2 p) {
    if (ring.empty() || distance(ring.back(), p) > 0)
        ring.push_back(p);
}

double segmentWidth(const PolylineData& pl, const PolyVertex& v, bool start) {
    if (v.startWidth > 0 || v.endWidth > 0)
        return start ? v.startWidth : v.endWidth;
    return pl.constantWidth;
}

bool hasWidth(const PolylineData& pl) {
    if (pl.constantWidth > 0)
        return true;
    return std::any_of(pl.v.begin(), pl.v.end(),
                       [](const PolyVertex& v) { return v.startWidth > 0 || v.endWidth > 0; });
}

void polylineDisplay(const PolylineData& pl, double tol, const DisplayStyle& style,
                     DisplayList& out) {
    if (pl.v.empty())
        return;
    if (pl.v.size() == 1) {
        DisplayPoints pts;
        pts.pts.push_back(pl.v[0].p);
        pts.style = style;
        out.items.push_back(std::move(pts));
        return;
    }
    std::vector<Segment> segs = segments(pl);
    if (!hasWidth(pl)) {
        DisplayPolyline d;
        d.style = style;
        d.closed = pl.closed;
        d.pts.push_back(pl.v[0].p);
        for (const Segment& s : segs) {
            if (s.curve.kind() == CurveRef::Kind::Line) {
                d.pts.push_back(s.p1);
            } else {
                std::vector<Vec2> pts;
                flatten(s.curve, tol, pts);
                if (s.reversed)
                    std::reverse(pts.begin(), pts.end());
                for (std::size_t i = 1; i < pts.size(); ++i)
                    d.pts.push_back(pts[i]);
                d.pts.back() = s.p1;
            }
        }
        if (d.closed && d.pts.size() > 1 && distance(d.pts.front(), d.pts.back()) <= 0)
            d.pts.pop_back();
        out.items.push_back(std::move(d));
        return;
    }
    // Wide segments become filled strips; zero-width segments stay thin lines.
    for (const Segment& s : segs) {
        const PolyVertex& va = pl.v[static_cast<std::size_t>(s.index)];
        double w0 = segmentWidth(pl, va, true), w1 = segmentWidth(pl, va, false);
        if (!(w0 > 0) && !(w1 > 0)) {
            DisplayPolyline d;
            d.style = style;
            std::vector<Vec2> pts;
            flatten(s.curve, tol, pts);
            if (s.reversed)
                std::reverse(pts.begin(), pts.end());
            pts.front() = s.p0;
            pts.back() = s.p1;
            d.pts = std::move(pts);
            out.items.push_back(std::move(d));
            continue;
        }
        std::vector<Vec2> pts;
        std::vector<double> ts;
        flatten(s.curve, tol, pts, &ts);
        std::vector<double> us(ts.size());
        for (std::size_t i = 0; i < ts.size(); ++i)
            us[i] = s.curve.kind() == CurveRef::Kind::Line ? s.toLocal(ts[i]) : s.toLocal(ts[i]);
        if (s.reversed) {
            std::reverse(pts.begin(), pts.end());
            std::reverse(us.begin(), us.end());
        }
        DisplayFill fill;
        fill.style = style;
        std::vector<Vec2> left, right;
        for (std::size_t i = 0; i < pts.size(); ++i) {
            Vec2 tan;
            if (i + 1 < pts.size())
                tan = pts[i + 1] - pts[i];
            else
                tan = pts[i] - pts[i - 1];
            Vec2 n = perp(normalized(tan));
            double w = w0 + (w1 - w0) * us[i];
            left.push_back(pts[i] + n * (0.5 * w));
            right.push_back(pts[i] - n * (0.5 * w));
        }
        std::vector<Vec2> ring;
        for (const Vec2& p : left)
            pushRing(ring, p);
        for (auto it = right.rbegin(); it != right.rend(); ++it)
            pushRing(ring, *it);
        fill.rings.push_back(std::move(ring));
        out.items.push_back(std::move(fill));
    }
}

BBox2 displayBounds(const DisplayList& list) {
    BBox2 b;
    for (const DisplayItem& it : list.items) {
        std::visit(
            [&](const auto& item) {
                using T = std::decay_t<decltype(item)>;
                if constexpr (std::is_same_v<T, DisplayFill>) {
                    for (const auto& r : item.rings)
                        for (const Vec2& p : r)
                            b.expand(p);
                } else {
                    for (const Vec2& p : item.pts)
                        b.expand(p);
                }
            },
            it);
    }
    return b;
}

}  // namespace

void DisplayRegistry::set(EntityKind kind, DisplayFn display, BoundsFn bounds) {
    if (isBuiltIn(kind))
        return;
    Registry& r = registry();
    std::unique_lock lock(r.mutex);
    r.entries[static_cast<std::size_t>(kind)] = {std::move(display), std::move(bounds)};
}

void DisplayRegistry::clear(EntityKind kind) {
    Registry& r = registry();
    std::unique_lock lock(r.mutex);
    r.entries[static_cast<std::size_t>(kind)] = {};
}

bool DisplayRegistry::has(EntityKind kind) {
    if (isBuiltIn(kind))
        return true;
    Registry& r = registry();
    std::shared_lock lock(r.mutex);
    return static_cast<bool>(r.entries[static_cast<std::size_t>(kind)].display);
}

DisplayStyle resolveStyle(const Document& doc, const Entity& e, const DisplayContext& ctx) {
    const EntityCommon& c = e.common;
    EffectiveLayer layer = doc.effective(c.layer);
    DisplayStyle s;
    switch (c.color.kind) {
        case ColorKind::ByBlock:
            s.color = ctx.byBlockColor;
            break;
        case ColorKind::Index:
        case ColorKind::Rgb:
            s.color = toRgba(c.color);
            break;
        default:
            s.color = toRgba(layer.color);
            break;
    }
    auto weightMm = [&](const LineWeight& w) {
        if (w.isValue())
            return w.toMm();
        return ctx.defaultLineweightMm;
    };
    if (c.lineweight.isByBlock())
        s.lineweight = ctx.byBlockLineweightMm;
    else if (c.lineweight.isByLayer())
        s.lineweight = weightMm(layer.lineweight);
    else
        s.lineweight = weightMm(c.lineweight);
    const std::vector<double>* pattern = nullptr;
    if (c.linetype == kLinetypeByBlock) {
        pattern = ctx.byBlockPattern;
    } else {
        LinetypeId id = c.linetype == kLinetypeByLayer ? layer.linetype : c.linetype;
        if (const Linetype* lt = doc.linetype(id))
            if (!lt->pattern.empty())
                pattern = &lt->pattern;
    }
    s.pattern = pattern;
    s.patternScale = c.linetypeScale * doc.header().ltScale;
    s.layer = c.layer;
    return s;
}

void buildDisplay(const Document& doc, const Entity& e, const DisplayContext& ctx,
                  DisplayList& out) {
    double tol = ctx.tolerance();
    switch (kind(e)) {
        case EntityKind::Line: {
            const auto& d = *entityAs<LineData>(e);
            DisplayPolyline p;
            p.pts = {d.a, d.b};
            p.style = resolveStyle(doc, e, ctx);
            out.items.push_back(std::move(p));
            return;
        }
        case EntityKind::Circle:
        case EntityKind::Arc:
        case EntityKind::Ellipse:
        case EntityKind::Spline: {
            std::vector<CurveRef> cs;
            curvesOf(e, cs);
            if (cs.empty())
                return;
            DisplayPolyline p;
            p.style = resolveStyle(doc, e, ctx);
            flatten(cs[0], tol, p.pts);
            if (cs[0].kind() != CurveRef::Kind::Spline && cs[0].isClosed()) {
                p.closed = true;
                p.pts.pop_back();
            }
            out.items.push_back(std::move(p));
            return;
        }
        case EntityKind::Polyline:
            polylineDisplay(*entityAs<PolylineData>(e), tol, resolveStyle(doc, e, ctx), out);
            return;
        case EntityKind::Point: {
            DisplayPoints p;
            p.pts = {entityAs<PointData>(e)->p};
            p.style = resolveStyle(doc, e, ctx);
            out.items.push_back(std::move(p));
            return;
        }
        case EntityKind::Proxy: {
            const auto& d = *entityAs<ProxyData>(e);
            out.items.insert(out.items.end(), d.graphics.items.begin(), d.graphics.items.end());
            return;
        }
        default:
            break;
    }
    DisplayFn fn;
    {
        Registry& r = registry();
        std::shared_lock lock(r.mutex);
        fn = r.entries[static_cast<std::size_t>(kind(e))].display;
    }
    if (fn)
        fn(doc, e, ctx, out);
}

BBox2 bounds(const Entity& e) {
    switch (kind(e)) {
        case EntityKind::Line: {
            const auto& d = *entityAs<LineData>(e);
            BBox2 b;
            b.expand(d.a);
            b.expand(d.b);
            return b;
        }
        case EntityKind::Point: {
            BBox2 b;
            b.expand(entityAs<PointData>(e)->p);
            return b;
        }
        case EntityKind::Circle:
        case EntityKind::Arc:
        case EntityKind::Ellipse:
        case EntityKind::Spline: {
            std::vector<CurveRef> cs;
            curvesOf(e, cs);
            BBox2 b;
            for (const CurveRef& c : cs)
                b.merge(c.bounds());
            return b;
        }
        case EntityKind::Polyline: {
            const auto& pl = *entityAs<PolylineData>(e);
            BBox2 b;
            for (const PolyVertex& v : pl.v)
                b.expand(v.p);
            for (const Segment& s : segments(pl))
                if (s.curve.kind() != CurveRef::Kind::Line)
                    b.merge(s.curve.bounds());
            double w = pl.constantWidth;
            for (const PolyVertex& v : pl.v)
                w = std::max({w, v.startWidth, v.endWidth});
            return w > 0 ? b.inflated(0.5 * w) : b;
        }
        case EntityKind::Proxy:
            return displayBounds(entityAs<ProxyData>(e)->graphics);
        default:
            return BBox2{};
    }
}

BBox2 bounds(const Entity& e, const Document& doc) {
    if (isBuiltIn(kind(e)))
        return bounds(e);
    RegistryEntry entry;
    {
        Registry& r = registry();
        std::shared_lock lock(r.mutex);
        entry = r.entries[static_cast<std::size_t>(kind(e))];
    }
    if (entry.bounds)
        return entry.bounds(doc, e);
    if (entry.display) {
        DisplayList list;
        entry.display(doc, e, DisplayContext{}, list);
        return displayBounds(list);
    }
    return BBox2{};
}

}  // namespace tbcad::geom
