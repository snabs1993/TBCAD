#include <algorithm>
#include <cmath>

#include "tbcad/geom/geom.h"
#include "tbcad/hatch/hatch.h"

namespace tbcad::hatch {

namespace {

constexpr std::uint8_t kToneAlpha = 110;

Rgba toRgba(const Color& c, const Rgba& base, const geom::DisplayContext& ctx) {
    switch (c.kind) {
        case ColorKind::Index:
            return paletteColor(c.indexValue());
        case ColorKind::Rgb:
            return Rgba{c.r, c.g, c.b, 255};
        case ColorKind::ByBlock:
            return ctx.byBlockColor;
        default:
            return base;
    }
}

Rgba fillColor(const HatchData& h, const Rgba& base, const geom::DisplayContext& ctx) {
    return toRgba(h.fill, base, ctx);
}

Rgba mix(const Rgba& a, const Rgba& b, double t) {
    auto ch = [&](std::uint8_t x, std::uint8_t y) {
        return static_cast<std::uint8_t>(std::lround(x + (y - x) * t));
    };
    return Rgba{ch(a.r, b.r), ch(a.g, b.g), ch(a.b, b.b), ch(a.a, b.a)};
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

    // FILL off: a solid, gradient or dense hatch shows its boundary only.
    auto outlines = [&](std::vector<std::vector<Vec2>> rings) {
        for (auto& ring : rings) {
            DisplayPolyline pl;
            pl.pts = std::move(ring);
            pl.closed = true;
            pl.style = style;
            out.items.emplace_back(std::move(pl));
        }
    };
    if (!ctx.fillMode && h->gradient) {
        Pattern solid;
        solid.solid = true;
        outlines(std::move(generateFill(*h, solid, FillOptions{}).rings));
        return;
    }

    if (h->gradient) {
        Rgba c1 = toRgba(h->gradient->color1, style.color, ctx);
        Rgba c2 = toRgba(h->gradient->color2, style.color, ctx);
        for (GradientBand& band : gradientBands(*h, ctx.tolerance())) {
            DisplayFill fill;
            fill.rings = std::move(band.rings);
            fill.style = style;
            fill.style.color = mix(c1, c2, band.t);
            out.items.emplace_back(std::move(fill));
        }
        return;
    }

    FillOptions opt;
    opt.tolerance = ctx.tolerance();
    opt.pixelSizeMm = ctx.pixelSizeMm;
    if (ctx.pixelSizeMm > 0) {
        // The screen cannot show more dashes than its area holds: a hatch covering a few pixels
        // gets a small budget, so thousands of them in view stay cheap.
        const BBox2 box = hatchBounds(*h);
        const double px = box.empty() ? 0 : box.width() / ctx.pixelSizeMm;
        const double py = box.empty() ? 0 : box.height() / ctx.pixelSizeMm;
        const double budget =
            std::clamp(px * py / 16.0, 2000.0, static_cast<double>(opt.maxSegments));
        opt.maxSegments = static_cast<std::size_t>(budget);
    }
    const Pattern* pattern = PatternRegistry::find(h->pattern);
    Pattern solidTone;
    solidTone.solid = true;
    // Annotative: the pattern scale is the size on paper; the model shows it at the annotation
    // scale (1:50 draws the pattern 50 times larger).
    HatchData annotated;
    const HatchData* hd = h;
    if (e.common.annotative && !h->solid) {
        const double f = annotationFactor(doc.header(), e, ctx.annoScale);
        if (f > 0 && std::isfinite(f) && f != 1) {
            annotated = *h;
            annotated.patternScale *= f;
            hd = &annotated;
        }
    }
    FillResult r = generateFill(*hd, pattern ? *pattern : solidTone, opt);
    // An unknown pattern name reads as a tone, not as an error on every redraw.
    bool tone = r.mode == FillResult::Mode::SolidTone || (!pattern && !h->solid);
    switch (r.mode) {
        case FillResult::Mode::Solid:
        case FillResult::Mode::SolidTone: {
            if (r.rings.empty())
                return;
            if (!ctx.fillMode) {
                outlines(std::move(r.rings));
                return;
            }
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
    // Doubling, not the exact size: a block's list collects many hatches one call at a time.
    if (const std::size_t want = out.items.size() + r.segments.size() + 1;
        out.items.capacity() < want)
        out.items.reserve(std::max(want, out.items.capacity() * 2));
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
