#include <algorithm>
#include <array>
#include <cmath>
#include <mutex>
#include <shared_mutex>

#include "linetype_marks.h"
#include "tbcad/geom/entity.h"

namespace tbcad::geom {

namespace {

struct RegistryEntry {
    DisplayFn display{};
    BoundsFn bounds{};
};

struct Registry {
    std::shared_mutex mutex;
    std::array<RegistryEntry, kEntityKindCount> entries{};
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
        case EntityKind::Ray:
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
    if (pl.restartLinetype && !hasWidth(pl) && style.pattern && !style.pattern->empty()) {
        // Ltype gen off: every segment is its own line, so the pattern starts again at each vertex.
        for (const Segment& s : segments(pl)) {
            DisplayPolyline d;
            d.style = style;
            if (s.curve.kind() == CurveRef::Kind::Line) {
                d.pts = {s.p0, s.p1};
            } else {
                flatten(s.curve, tol, d.pts);
                if (s.reversed)
                    std::reverse(d.pts.begin(), d.pts.end());
                d.pts.front() = s.p0;
                d.pts.back() = s.p1;
            }
            out.items.push_back(std::move(d));
        }
        return;
    }
    if (!hasWidth(pl)) {
        bool straight = true;
        for (const PolyVertex& v : pl.v)
            straight = straight && v.bulge == 0;
        if (straight) {  // the common case: the vertices are the outline
            DisplayPolyline d;
            d.style = style;
            d.closed = pl.closed;
            const std::size_t n = pl.v.size();
            const std::size_t count = static_cast<std::size_t>(segmentCount(pl));
            d.pts.reserve(count + 1);
            d.pts.push_back(pl.v[0].p);
            for (std::size_t i = 0; i < count; ++i) {
                const Vec2 a = pl.v[i].p, b = pl.v[(i + 1) % n].p;
                if (a.x == b.x && a.y == b.y)
                    continue;  // a zero-length segment is skipped, as in segments()
                d.pts.push_back(b);
            }
            if (d.closed && d.pts.size() > 1 && distance(d.pts.front(), d.pts.back()) <= 0)
                d.pts.pop_back();
            out.items.push_back(std::move(d));
            return;
        }
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
    // Wide segments become filled strips; zero-width segments stay thin lines. A strip ends square,
    // so the corner between two strips is filled with a miter (or a bevel when it is too sharp).
    struct Ends {
        bool wide = false;
        Vec2 startTan{}, endTan{}, p0{}, p1{};
        double w0 = 0, w1 = 0;
    };
    std::vector<Ends> ends(segs.size());
    std::size_t segIndex = 0;
    for (const Segment& s : segs) {
        Ends& en = ends[segIndex++];
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
        if (pts.size() >= 2) {
            en.wide = true;
            en.startTan = pts[1] - pts[0];
            en.endTan = pts.back() - pts[pts.size() - 2];
            en.p0 = s.p0;
            en.p1 = s.p1;
            en.w0 = w0;
            en.w1 = w1;
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
    const std::size_t n = ends.size();
    for (std::size_t i = 0; i < n; ++i) {
        const bool last = i + 1 == n;
        if (last && !pl.closed)
            break;
        const Ends& a = ends[i];
        const Ends& b = ends[last ? 0 : i + 1];
        if (!a.wide || !b.wide || distance(a.p1, b.p0) > 1e-9)
            continue;
        Vec2 t1 = normalized(a.endTan), t2 = normalized(b.startTan);
        const double turn = cross(t1, t2);
        if (std::abs(turn) < 1e-9)
            continue;  // straight on: nothing to fill
        // Outer side of the turn: right of the path for a left turn, left for a right turn.
        const double side = turn > 0 ? -1.0 : 1.0;
        const Vec2 n1 = perp(t1) * side, n2 = perp(t2) * side;
        const Vec2 pa = a.p1 + n1 * (0.5 * a.w1);
        const Vec2 pb = b.p0 + n2 * (0.5 * b.w0);
        // Miter point: intersection of the two outer edges.
        Vec2 corner = pa;
        bool miter = false;
        const double denom = cross(t1, t2);
        if (std::abs(denom) > 1e-9) {
            const double k = cross(pb - pa, t2) / denom;
            const Vec2 m = pa + t1 * k;
            const double halfMax = 0.5 * std::max(a.w1, b.w0);
            if (halfMax > 0 && distance(m, a.p1) <= 4.0 * halfMax) {
                corner = m;
                miter = true;
            }
        }
        DisplayFill join;
        join.style = style;
        std::vector<Vec2> ring{a.p1, pa};
        if (miter)
            ring.push_back(corner);
        ring.push_back(pb);
        join.rings.push_back(std::move(ring));
        out.items.push_back(std::move(join));
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

int resolveTransparency(const Document& doc, const Entity& e, const DisplayContext& ctx) {
    if (!ctx.showTransparency)
        return 0;
    const std::int8_t t = e.common.transparency;
    int v = 0;
    if (t == kTransparencyByBlock)
        v = ctx.byBlockTransparency;
    else if (t == kTransparencyByLayer)
        v = doc.effective(e.common.layer).transparency;
    else
        v = t;
    return std::clamp(v, 0, static_cast<int>(kTransparencyMax));
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
    if (const int tr = resolveTransparency(doc, e, ctx); tr > 0 && !isBackgroundColor(s.color))
        s.color.a = static_cast<std::uint8_t>(std::lround(s.color.a * (100 - tr) / 100.0));
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
    const std::vector<LinetypeMark>* marks = nullptr;
    if (c.linetype == kLinetypeByBlock) {
        pattern = ctx.byBlockPattern;
    } else {
        LinetypeId id = c.linetype == kLinetypeByLayer ? layer.linetype : c.linetype;
        if (const Linetype* lt = doc.linetype(id))
            if (!lt->pattern.empty()) {
                pattern = &lt->pattern;
                if (!lt->marks.empty())
                    marks = &lt->marks;
            }
    }
    s.pattern = pattern;
    // Linetypes in a viewport follow the viewport scale unless PSLTSCALE is off; in model space
    // they follow the annotation scale when MSLTSCALE is on.
    double view = 1;
    if (ctx.annoScale > 0)
        view = psLtScale(doc.header()) ? ctx.annoScale : 1;
    else if (!ctx.paperSpace && msLtScale(doc.header()))
        view = currentAnnotationScale(doc.header());
    s.patternScale = c.linetypeScale * doc.header().ltScale * view;
    s.marks = marks;
    s.layer = c.layer;
    return s;
}

namespace {
void buildDisplayItems(const Document& doc, const Entity& e, const DisplayContext& ctx,
                       DisplayList& out);
}

std::vector<AnnotationGhost> annotationGhosts(const Document& doc, std::span<const EntityId> ids,
                                              const DisplayContext& ctx) {
    std::vector<AnnotationGhost> out;
    if (!selectionAnnoDisplay(doc.header()))
        return out;
    for (EntityId id : ids) {
        const Entity* e = doc.find(id);
        if (!e || !e->common.annotative || e->common.annoScales.size() < 2 ||
            kind(*e) == EntityKind::Insert)
            continue;
        const double shown = annotationFactor(doc.header(), e->common, ctx.annoScale);
        for (double n : e->common.annoScales) {
            if (n == shown || !(n > 0))
                continue;
            DisplayContext at = ctx;
            at.annoScale = n;
            AnnotationGhost g;
            g.id = id;
            g.scale = n;
            buildDisplay(doc, *e, at, g.display);
            if (!g.display.items.empty())
                out.push_back(std::move(g));
        }
    }
    return out;
}

void buildDisplay(const Document& doc, const Entity& e, const DisplayContext& ctx,
                  DisplayList& out) {
    const std::size_t from = out.items.size();
    buildDisplayItems(doc, e, ctx, out);
    // Linetypes with text or shapes: those are drawn along each new dashed path.
    DisplayList marks;
    for (std::size_t i = from; i < out.items.size(); ++i)
        if (const auto* p = std::get_if<DisplayPolyline>(&out.items[i]))
            if (p->style.marks)
                appendLinetypeMarks(doc, ctx, *p, marks);
    for (DisplayItem& item : marks.items)
        out.items.push_back(std::move(item));
    const EntityKind k = kind(e);
    if (ctx.quickText && (k == EntityKind::Text || k == EntityKind::MText)) {
        BBox2 box;
        std::vector<DisplayItem> kept;
        DisplayStyle style{};
        bool any = false;
        for (std::size_t i = from; i < out.items.size(); ++i) {
            if (const auto* f = std::get_if<DisplayFill>(&out.items[i])) {
                if (isBackgroundColor(f->style.color)) {  // a text background mask stays
                    kept.push_back(std::move(out.items[i]));
                    continue;
                }
                for (const auto& ring : f->rings)
                    for (const Vec2& p : ring)
                        box.expand(p);
                if (!any)
                    style = f->style;
                any = true;
            } else {  // underline and overline strokes stay
                kept.push_back(std::move(out.items[i]));
            }
        }
        if (any && !box.empty()) {
            DisplayPolyline p;
            p.pts = {box.min, {box.max.x, box.min.y}, box.max, {box.min.x, box.max.y}};
            p.closed = true;
            p.style = style;
            kept.push_back(std::move(p));
        }
        out.items.resize(from);
        for (DisplayItem& item : kept)
            out.items.push_back(std::move(item));
    }
    // Text glyphs are fills too, so only wide polylines are touched here; hatches do it
    // themselves.
    const bool fillKind = k == EntityKind::Polyline;
    if (!ctx.fillMode && fillKind) {
        // FILL off: each filled area is shown by the outline of its rings. A mask (wipeout, drawn
        // in the background colour) still hides what lies behind it.
        std::vector<DisplayItem> kept;
        kept.reserve(out.items.size() - from);
        for (std::size_t i = from; i < out.items.size(); ++i) {
            auto* fill = std::get_if<DisplayFill>(&out.items[i]);
            if (!fill || isBackgroundColor(fill->style.color)) {
                kept.push_back(std::move(out.items[i]));
                continue;
            }
            for (auto& ring : fill->rings) {
                DisplayPolyline p;
                p.pts = std::move(ring);
                p.closed = true;
                p.style = fill->style;
                p.style.pattern = nullptr;
                p.style.marks = nullptr;
                kept.push_back(std::move(p));
            }
        }
        out.items.resize(from);
        for (DisplayItem& item : kept)
            out.items.push_back(std::move(item));
    }
}

namespace {
void buildDisplayItems(const Document& doc, const Entity& e, const DisplayContext& ctx,
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
        case EntityKind::Polyline: {
            const auto& pl = *entityAs<PolylineData>(e);
            if (e.common.wipeout && pl.closed && pl.v.size() >= 3) {
                // A mask: filled with the background colour, no outline.
                DisplayList outline;
                polylineDisplay(pl, tol, resolveStyle(doc, e, ctx), outline);
                const int frame = wipeoutFrameMode(doc.header());
                std::vector<DisplayItem> frames;
                for (auto& item : outline.items) {
                    if (auto* line = std::get_if<DisplayPolyline>(&item)) {
                        DisplayFill fill;
                        fill.rings.push_back(line->pts);
                        fill.style = line->style;
                        fill.style.color = kBackgroundColor;
                        out.items.push_back(std::move(fill));
                        if (frame != 0) {  // the outline, above the mask
                            line->closed = true;
                            line->style.noPlot = frame == 2;
                            frames.push_back(std::move(*line));
                        }
                    }
                }
                for (DisplayItem& f : frames)
                    out.items.push_back(std::move(f));
                return;
            }
            polylineDisplay(pl, tol, resolveStyle(doc, e, ctx), out);
            return;
        }
        case EntityKind::Point: {
            DisplayPoints p;
            p.pts = {entityAs<PointData>(e)->p};
            p.style = resolveStyle(doc, e, ctx);
            out.items.push_back(std::move(p));
            return;
        }
        case EntityKind::Ray: {
            const auto& d = *entityAs<RayData>(e);
            DisplayPolyline p;
            p.pts = {rayStart(d), rayEnd(d)};
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
}  // namespace

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
            // Straight segments add nothing to the box of their vertices; only arcs are looked at.
            bool anyBulge = false;
            for (const PolyVertex& v : pl.v)
                anyBulge = anyBulge || v.bulge != 0;
            if (anyBulge)
                for (const Segment& s : segments(pl))
                    if (s.curve.kind() != CurveRef::Kind::Line)
                        b.merge(s.curve.bounds());
            double w = pl.constantWidth;
            for (const PolyVertex& v : pl.v)
                w = std::max({w, v.startWidth, v.endWidth});
            return w > 0 ? b.inflated(0.5 * w) : b;
        }
        case EntityKind::Ray: {
            const auto& d = *entityAs<RayData>(e);
            BBox2 b;
            b.expand(rayStart(d));
            b.expand(rayEnd(d));
            return b;
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
