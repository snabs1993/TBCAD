#include <algorithm>
#include <cmath>

#include "text_internal.h"

namespace tbcad::text {

namespace {

// Flattening tolerance for outlines, in em, quantised to powers of two so the font library's
// per-tolerance cache stays small.
double outlineTolerance(double tolMm, double mmPerEm) {
    double want = mmPerEm > 0 ? tolMm / mmPerEm : font::kDefaultFlattenTolerance;
    double t = font::kDefaultFlattenTolerance;
    while (t * 2 <= want && t < 0.02)
        t *= 2;
    return t;
}

}  // namespace

void buildTextDisplay(const Document& doc, const Entity& e, const geom::DisplayContext& ctx,
                      DisplayList& out) {
    TextLayout layout = layoutEntity(doc, e, ctx.annoScale);
    if (layout.empty() && layout.lines.empty())
        return;
    const DisplayStyle base = geom::resolveStyle(doc, e, ctx);
    const double px = ctx.pixelSizeMm > 0 ? ctx.pixelSizeMm : 0.1;

    // MTEXT background mask: the frame grown by (bgScale - 1) text heights. In the drawing
    // background colour it hides what lies behind, like a wipeout.
    if (const auto* m = entityAs<MTextData>(e); m && m->bgMode && !layout.frame.empty()) {
        const double h = layout.lines.empty() ? m->height : layout.lines.front().height;
        const double grow = std::max(0.0, m->bgScale - 1.0) * (h > 0 ? h : m->height);
        const BBox2 f = layout.frame.inflated(grow);
        DisplayFill bg;
        bg.style = base;
        bg.style.color = m->bgMode == 2 && m->bgColor >= 1 && m->bgColor <= 255
                             ? paletteColor(static_cast<std::uint8_t>(m->bgColor))
                             : kBackgroundColor;
        std::vector<Vec2> ring;
        for (Vec2 c : {Vec2{f.min.x, f.min.y}, Vec2{f.max.x, f.min.y}, Vec2{f.max.x, f.max.y},
                       Vec2{f.min.x, f.max.y}})
            ring.push_back(layout.toWorld.apply(c));
        bg.rings.push_back(std::move(ring));
        out.items.emplace_back(std::move(bg));
    }

    // Capital height of the largest line on screen, in pixels.
    double scaleToWorld = std::sqrt(std::abs(layout.toWorld.determinant()));
    double maxH = 0;
    for (const LayoutLine& l : layout.lines)
        maxH = std::max(maxH, l.height);
    if (maxH * scaleToWorld / px < kPlaceholderPixels) {
        for (const LayoutLine& l : layout.lines) {
            if (l.endGlyph == l.firstGlyph)
                continue;
            DisplayPolyline box;
            box.closed = true;
            box.style = base;
            const double x0 = l.x0, x1 = l.x0 + l.width, y0 = l.baseline,
                         y1 = l.baseline + l.height;
            for (Vec2 c : {Vec2{x0, y0}, Vec2{x1, y0}, Vec2{x1, y1}, Vec2{x0, y1}})
                box.pts.push_back(layout.toWorld.apply(c));
            out.items.emplace_back(std::move(box));
        }
        return;
    }

    for (const LayoutLine& l : layout.lines)
        for (const LayoutRule& r : l.rules) {
            DisplayPolyline rule;
            rule.style = base;
            rule.pts = {layout.toWorld.apply({r.x0, r.y}), layout.toWorld.apply({r.x1, r.y})};
            out.items.emplace_back(std::move(rule));
        }

    font::FontId lastFont = kNoFont;
    font::FontLibrary::FileData file;
    for (const PlacedGlyph& g : layout.glyphs) {
        if (fonts().isStroke(g.font)) {
            for (const font::StrokePath& sp : fonts().strokes(g.font, g.glyphId).paths) {
                if (sp.points.size() < 2)
                    continue;
                DisplayPolyline line;
                line.style = base;
                if (g.color >= 1 && g.color <= 255)
                    line.style.color = paletteColor(static_cast<std::uint8_t>(g.color));
                line.pts.reserve(sp.points.size());
                for (const font::FPoint& p : sp.points) {
                    Vec2 local{g.origin.x + (p.x * g.xScale + g.shear * p.y) * g.scale,
                               g.origin.y + p.y * g.scale};
                    line.pts.push_back(layout.toWorld.apply(local));
                }
                out.items.emplace_back(std::move(line));
            }
            continue;
        }
        const font::GlyphOutline& o = fonts().outline(
            g.font, g.glyphId, outlineTolerance(ctx.tolerance(), g.scale * scaleToWorld));
        if (o.contours.empty())
            continue;
        DisplayFill fill;
        fill.style = base;
        fill.glyph = true;
        if (g.color >= 1 && g.color <= 255)
            fill.style.color = paletteColor(static_cast<std::uint8_t>(g.color));
        for (const font::Contour& c : o.contours) {
            if (c.points.size() < 3)
                continue;
            std::vector<Vec2> ring;
            ring.reserve(c.points.size());
            for (const font::FPoint& p : c.points) {
                Vec2 local{g.origin.x + (p.x * g.xScale + g.shear * p.y) * g.scale,
                           g.origin.y + p.y * g.scale};
                ring.push_back(layout.toWorld.apply(local));
            }
            fill.rings.push_back(std::move(ring));
        }
        if (fill.rings.empty())
            continue;
        // The same glyph as real text, for outputs that can embed the font (PDF).
        Transform2 em;
        em.m00 = g.scale * g.xScale;
        em.m01 = g.shear * g.scale;
        em.tx = g.origin.x;
        em.m11 = g.scale;
        em.ty = g.origin.y;
        if (g.font != lastFont) {
            file = fonts().fileData(g.font);
            lastFont = g.font;
        }
        if (file.bytes)
            fill.glyphRef = GlyphRef{file.bytes->data(),
                                     file.bytes->size(),
                                     static_cast<std::uint32_t>(file.faceIndex),
                                     g.glyphId,
                                     g.text,
                                     layout.toWorld * em};
        out.items.emplace_back(std::move(fill));
    }
}

bool hitTest(const Document& doc, const Entity& e, Vec2 p, double aperture) {
    TextLayout layout = layoutEntity(doc, e);
    if (layout.frame.empty())
        return false;
    auto inv = layout.toWorld.inverse();
    if (!inv)
        return false;
    Vec2 q = inv.value().apply(p);
    double k = std::sqrt(std::abs(layout.toWorld.determinant()));
    return layout.box.inflated(k > 0 ? aperture / k : aperture).contains(q);
}

void registerDisplay() {
    auto display = [](const Document& doc, const Entity& e, const geom::DisplayContext& ctx,
                      DisplayList& out) { buildTextDisplay(doc, e, ctx, out); };
    auto bounds = [](const Document& doc, const Entity& e) {
        TextLayout l = layoutEntity(doc, e);
        BBox2 b = l.worldBounds();
        if (b.empty()) {
            if (const auto* t = entityAs<TextData>(e))
                b.expand(t->position);
            else if (const auto* m = entityAs<MTextData>(e))
                b.expand(m->position);
        }
        return b;
    };
    geom::DisplayRegistry::set(EntityKind::Text, display, bounds);
    geom::DisplayRegistry::set(EntityKind::MText, display, bounds);
}

}  // namespace tbcad::text
