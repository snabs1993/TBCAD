#include <algorithm>
#include <cmath>

#include "tbcad/geom/geom.h"
#include "tbcad/hatch/hatch.h"

namespace tbcad::hatch {

namespace {

constexpr std::uint8_t kToneAlpha = 110;

Rgba fillColor(const HatchData& h, const Rgba& base, const geom::DisplayContext& ctx) {
    switch (h.fill.kind) {
        case ColorKind::Index:
            return paletteColor(h.fill.indexValue());
        case ColorKind::Rgb:
            return Rgba{h.fill.r, h.fill.g, h.fill.b, 255};
        case ColorKind::ByBlock:
            return ctx.byBlockColor;
        default:
            return base;
    }
}

void hatchDisplay(const Document& doc, const Entity& e, const geom::DisplayContext& ctx,
                  DisplayList& out) {
    const HatchData* h = entityAs<HatchData>(e);
    if (!h)
        return;
    DisplayStyle style = geom::resolveStyle(doc, e, ctx);
    style.color = fillColor(*h, style.color, ctx);
    style.pattern = nullptr;  // hatch lines are continuous; the pattern makes the dashes
    if (!e.common.lineweight.isValue())
        style.lineweight = 0;  // thinnest, unless the hatch has a weight of its own

    FillOptions opt;
    opt.tolerance = ctx.tolerance();
    opt.pixelSizeMm = ctx.pixelSizeMm;
    const Pattern* pattern = PatternRegistry::find(h->pattern);
    Pattern solidTone;
    solidTone.solid = true;
    FillResult r = generateFill(*h, pattern ? *pattern : solidTone, opt);
    // An unknown pattern name reads as a tone, not as an error on every redraw.
    bool tone = r.mode == FillResult::Mode::SolidTone || (!pattern && !h->solid);
    switch (r.mode) {
        case FillResult::Mode::Solid:
        case FillResult::Mode::SolidTone: {
            if (r.rings.empty())
                return;
            DisplayFill fill;
            fill.rings = std::move(r.rings);
            fill.style = style;
            if (tone)
                fill.style.color.a = kToneAlpha;
            out.items.emplace_back(std::move(fill));
            return;
        }
        case FillResult::Mode::Lines:
            break;
    }
    out.items.reserve(out.items.size() + r.segments.size() + 1);
    for (const auto& [a, b] : r.segments) {
        DisplayPolyline pl;
        pl.pts = {a, b};
        pl.style = style;
        out.items.emplace_back(std::move(pl));
    }
    if (!r.dots.empty()) {
        DisplayPoints pts;
        pts.pts = std::move(r.dots);
        pts.style = style;
        out.items.emplace_back(std::move(pts));
    }
}

BBox2 boundsOf(const Document&, const Entity& e) {
    const HatchData* h = entityAs<HatchData>(e);
    return h ? hatchBounds(*h) : BBox2{};
}

Entity entityWith(const Entity& hatch, EntityData data) {
    Entity e;
    e.common = hatch.common;
    e.common.id = {};
    const HatchData* h = entityAs<HatchData>(hatch);
    if (h && h->fill.kind != ColorKind::ByLayer)
        e.common.color = h->fill;
    e.data = std::move(data);
    return e;
}

}  // namespace

void registerHatchDisplay() {
    geom::DisplayRegistry::set(EntityKind::Hatch, hatchDisplay, boundsOf);
}

BBox2 hatchBounds(const HatchData& hatch) {
    BBox2 box;
    for (const HatchLoop& l : hatch.loops)
        for (const geom::CurveRef& c : loopCurves(l))
            box.merge(c.bounds());
    return box;
}

Entity makeHatchEntity(const Boundary& boundary, const Pattern& pattern, double scale,
                       double angleRadians, Vec2 origin) {
    HatchData d;
    d.loops = boundary.loops;
    d.solid = pattern.solid;
    d.pattern = pattern.name;
    d.patternScale = scale;
    d.patternAngle = angleRadians;
    d.patternOrigin = origin;
    d.sourceBoundary = boundary.sources;
    Entity e;
    e.data = Indirect<HatchData>(std::move(d));
    return e;
}

Result<std::vector<Entity>> explodeHatch(const Document& doc, const Entity& hatch,
                                         double tolerance) {
    (void)doc;
    const HatchData* h = entityAs<HatchData>(hatch);
    if (!h)
        return makeError(ErrorCode::InvalidArgument, "not a hatch");
    std::vector<Entity> out;
    const Pattern* pattern = PatternRegistry::find(h->pattern);
    bool solid = h->solid || !pattern || pattern->solid;
    if (!solid) {
        FillOptions opt;
        opt.tolerance = tolerance;
        FillResult r = generateFill(*h, *pattern, opt);
        if (r.mode != FillResult::Mode::Lines)
            return makeError(ErrorCode::OutOfRange,
                             "The pattern is too dense to explode into lines.");
        for (const auto& [a, b] : r.segments)
            out.push_back(entityWith(hatch, LineData{a, b}));
        for (const Vec2& p : r.dots)
            out.push_back(entityWith(hatch, PointData{p}));
        return out;
    }
    for (const HatchLoop& loop : h->loops) {
        for (const HatchEdge& edge : loop.edges) {
            std::visit(
                [&](const auto& e) {
                    using T = std::decay_t<decltype(e)>;
                    if constexpr (std::is_same_v<T, HatchLineEdge>) {
                        out.push_back(entityWith(hatch, LineData{e.a, e.b}));
                    } else if constexpr (std::is_same_v<T, HatchArcEdge>) {
                        double from = e.ccw ? e.startAngle : e.endAngle;
                        double to = e.ccw ? e.endAngle : e.startAngle;
                        if (tol::nearlyEqualAngle(from, to))
                            out.push_back(entityWith(hatch, CircleData{e.center, e.radius}));
                        else
                            out.push_back(
                                entityWith(hatch, ArcData{e.center, e.radius, normalizeAngle(from),
                                                          normalizeAngle(to)}));
                    } else if constexpr (std::is_same_v<T, HatchEllipseEdge>) {
                        double from = e.ccw ? e.startParam : e.endParam;
                        double to = e.ccw ? e.endParam : e.startParam;
                        EllipseData d;
                        d.center = e.center;
                        d.majorAxis = e.majorAxis;
                        d.ratio = e.ratio;
                        d.startParam = normalizeAngle(from);
                        d.endParam = tol::nearlyEqualAngle(from, to) ? d.startParam + kTwoPi
                                                                     : normalizeAngle(to);
                        out.push_back(entityWith(hatch, Indirect<EllipseData>(std::move(d))));
                    } else if constexpr (std::is_same_v<T, HatchSplineEdge>) {
                        out.push_back(entityWith(hatch, Indirect<SplineData>(e.spline)));
                    } else {
                        PolylineData pl;
                        pl.v = e.v;
                        pl.closed = e.closed;
                        out.push_back(entityWith(hatch, std::move(pl)));
                    }
                },
                edge);
        }
    }
    return out;
}

}  // namespace tbcad::hatch
