#include "tbcad/export/plot.h"

#include <algorithm>
#include <cmath>

#include "tbcad/geom/entity.h"

namespace tbcad::exportlib {

const std::vector<PaperSize>& paperSizes() {
    static const std::vector<PaperSize> sizes = {
        {"A0", 841, 1189},   {"A1", 594, 841},      {"A2", 420, 594},
        {"A3", 297, 420},    {"A4", 210, 297},      {"ANSI A", 215.9, 279.4},
        {"ANSI B", 279.4, 431.8}, {"ANSI C", 431.8, 558.8}, {"ANSI D", 558.8, 863.6},
        {"ANSI E", 863.6, 1117.6},
    };
    return sizes;
}

const PaperSize* findPaperSize(const std::string& name) {
    for (const auto& p : paperSizes())
        if (p.name == name)
            return &p;
    return nullptr;
}

const std::vector<double>& scalePresets() {
    static const std::vector<double> presets = {1, 2, 5, 10, 20, 50, 100, 200, 500, 1000};
    return presets;
}

namespace {

bool plottable(const Document& doc, const Entity& e) {
    if (!e.common.visible)
        return false;
    EffectiveLayer l = doc.effective(e.common.layer);
    return l.visible() && l.plot;
}

BBox2 extentsOf(const Document& doc) {
    BBox2 box;
    for (EntityId id : doc.modelSpace()) {
        const Entity* e = doc.find(id);
        if (e && plottable(doc, *e))
            box.merge(geom::bounds(*e, doc));
    }
    return box;
}

Error invalid(std::string msg) {
    return Error{ErrorCode::InvalidArgument, std::move(msg)};
}

// Dash pattern (+ dash, - gap, 0 dot; drawing units) to alternating on/off paper lengths.
std::vector<double> dashArray(const std::vector<double>& pattern, double scale, double dotMm) {
    std::vector<double> out;
    bool lastOn = false;  // true when the previous element was an "on" element
    for (double v : pattern) {
        if (v < 0) {
            if (!lastOn)
                out.push_back(0);
            out.push_back(-v * scale);
            lastOn = false;
        } else {
            if (lastOn)
                out.push_back(0);
            out.push_back(v == 0 ? dotMm : v * scale);
            lastOn = true;
        }
    }
    if (lastOn)
        out.push_back(0);
    if (out.size() % 2)
        out.push_back(0);
    double total = 0;
    for (double d : out)
        total += d;
    if (!(total > 1e-6))
        out.clear();
    return out;
}

struct Builder {
    const PlotSettings& s;
    const PlotLayout& layout;

    PlotStyle style(const DisplayStyle& ds) const {
        PlotStyle p;
        p.color = plotColor(ds.color, s.color);
        p.widthMm = (!s.lineweights || !(ds.lineweight > 0)) ? s.hairlineMm : ds.lineweight;
        if (ds.pattern && !ds.pattern->empty())
            p.dashMm = dashArray(*ds.pattern, ds.patternScale * layout.paperPerUnit, p.widthMm);
        return p;
    }
    Vec2 pt(Vec2 p) const { return layout.toPaper(p); }

    void item(const DisplayItem& it, std::vector<PlotPrimitive>& out) const {
        if (const auto* pl = std::get_if<DisplayPolyline>(&it)) {
            if (pl->pts.size() < 2)
                return;
            PlotPath path;
            path.closed = pl->closed;
            path.style = style(pl->style);
            path.pts.reserve(pl->pts.size());
            for (const Vec2& p : pl->pts)
                path.pts.push_back(pt(p));
            out.emplace_back(std::move(path));
        } else if (const auto* f = std::get_if<DisplayFill>(&it)) {
            PlotFill fill;
            fill.color = plotColor(f->style.color, s.color);
            for (const auto& ring : f->rings) {
                if (ring.size() < 3)
                    continue;
                std::vector<Vec2> r;
                r.reserve(ring.size());
                for (const Vec2& p : ring)
                    r.push_back(pt(p));
                fill.rings.push_back(std::move(r));
            }
            if (!fill.rings.empty())
                out.emplace_back(std::move(fill));
        } else if (const auto* d = std::get_if<DisplayPoints>(&it)) {
            PlotDots dots;
            dots.style = style(d->style);
            dots.style.dashMm.clear();
            for (const Vec2& p : d->pts)
                dots.pts.push_back(pt(p));
            if (!dots.pts.empty())
                out.emplace_back(std::move(dots));
        }
    }
};

}  // namespace

Rgba plotColor(Rgba c, ColorMode mode) {
    double lum = (0.2126 * c.r + 0.7152 * c.g + 0.0722 * c.b) / 255.0;
    Rgba base = c;
    base.a = 255;
    if (mode == ColorMode::Monochrome)
        return {0, 0, 0, 255};
    if (lum > 0.9)  // white lines would vanish on the white sheet
        return {0, 0, 0, 255};
    if (mode == ColorMode::Greyscale) {
        auto g = static_cast<std::uint8_t>(std::lround(lum * 255.0));
        return {g, g, g, 255};
    }
    return base;
}

Result<PlotLayout> computeLayout(const Document& doc, const PlotSettings& s) {
    PlotLayout lay;
    double w, h;
    if (s.paperName == "Custom") {
        w = s.customWidthMm;
        h = s.customHeightMm;
    } else if (const PaperSize* p = findPaperSize(s.paperName)) {
        w = p->shortMm;
        h = p->longMm;
    } else {
        return invalid("Unknown paper size: " + s.paperName);
    }
    if (!(w > 0) || !(h > 0))
        return invalid("Paper size must be positive");
    if (s.landscape != (w > h))
        std::swap(w, h);
    lay.pageWidthMm = w;
    lay.pageHeightMm = h;
    double m = std::max(0.0, s.marginMm);
    lay.printable.min = {m, m};
    lay.printable.max = {w - m, h - m};
    if (lay.printable.width() <= 0 || lay.printable.height() <= 0)
        return invalid("Margins leave no printable area");

    lay.area = s.area == PlotArea::Extents ? extentsOf(doc) : s.window;
    if (lay.area.empty())
        return invalid(s.area == PlotArea::Extents ? "Nothing to plot" : "The plot window is empty");
    double aw = lay.area.width(), ah = lay.area.height();
    if (!(aw > 0) && !(ah > 0)) {  // a single point: give it a 1 unit box
        lay.area = lay.area.inflated(0.5);
        aw = ah = 1;
    }
    const double pw = lay.printable.width(), ph = lay.printable.height();
    if (s.fitToPaper) {
        double sx = aw > 0 ? pw / aw : INFINITY;
        double sy = ah > 0 ? ph / ah : INFINITY;
        lay.paperPerUnit = std::min(sx, sy);
    } else {
        if (!(s.scaleN > 0))
            return invalid("The scale must be positive");
        lay.paperPerUnit = 1.0 / s.scaleN;
    }
    const double sw = aw * lay.paperPerUnit, sh = ah * lay.paperPerUnit;
    lay.overflows = sw > pw + 1e-6 || sh > ph + 1e-6;
    if (s.centerOnPaper)
        lay.origin = {lay.printable.min.x + (pw - sw) / 2, lay.printable.min.y + (ph - sh) / 2};
    else
        lay.origin = {lay.printable.min.x + s.offsetMm.x, lay.printable.min.y + s.offsetMm.y};
    return lay;
}

Result<PlotSheet> buildSheet(const Document& doc, const PlotSettings& s) {
    auto layout = computeLayout(doc, s);
    if (!layout)
        return layout.error();
    PlotSheet sheet;
    sheet.layout = layout.value();
    const PlotLayout& lay = sheet.layout;
    Builder b{s, lay};

    geom::DisplayContext ctx;
    ctx.chordTolMm = s.chordTolMm / lay.paperPerUnit;
    ctx.pixelSizeMm = ctx.chordTolMm * 4;
    ctx.defaultLineweightMm = 0.25;
    ctx.byBlockLineweightMm = 0.25;
    // Slight growth so lines on the border are not culled away.
    const BBox2 cull = lay.area.inflated(1.0 / lay.paperPerUnit);

    for (EntityId id : doc.modelSpace()) {
        const Entity* e = doc.find(id);
        if (!e || !plottable(doc, *e))
            continue;
        BBox2 box = geom::bounds(*e, doc);
        if (!box.empty() && !box.intersects(cull))
            continue;
        ++sheet.entityCount;
        DisplayStyle ds = geom::resolveStyle(doc, *e, ctx);
        if (kind(*e) == EntityKind::Circle || kind(*e) == EntityKind::Arc) {
            // Keep the arc hint: PDF and SVG draw these as Béziers / arcs, not chords.
            PlotArc arc;
            arc.style = b.style(ds);
            if (const auto* c = std::get_if<CircleData>(&e->data)) {
                arc.center = b.pt(c->center);
                arc.radius = c->radius * lay.paperPerUnit;
                arc.full = true;
            } else if (const auto* a = std::get_if<ArcData>(&e->data)) {
                arc.center = b.pt(a->center);
                arc.radius = a->radius * lay.paperPerUnit;
                arc.start = a->startAngle;
                arc.end = a->endAngle;
                while (arc.end <= arc.start)
                    arc.end += kTwoPi;
            }
            if (arc.radius > 0)
                sheet.prims.emplace_back(std::move(arc));
            continue;
        }
        DisplayList list;
        geom::buildDisplay(doc, *e, ctx, list);
        for (const DisplayItem& it : list.items)
            b.item(it, sheet.prims);
    }
    return sheet;
}

}  // namespace tbcad::exportlib
