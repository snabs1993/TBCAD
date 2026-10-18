#include "tbcad/export/plot.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <map>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include "tbcad/geom/entity.h"
#include "tbcad/underlay/image.h"

namespace tbcad::exportlib {

const std::vector<PaperSize>& paperSizes() {
    static const std::vector<PaperSize> sizes = {
        {"A0", 841, 1189},         {"A1", 594, 841},         {"A2", 420, 594},
        {"A3", 297, 420},          {"A4", 210, 297},         {"ANSI A", 215.9, 279.4},
        {"ANSI B", 279.4, 431.8},  {"ANSI C", 431.8, 558.8}, {"ANSI D", 558.8, 863.6},
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

// AutoCAD never plots Defpoints (dimension definition points), whatever the layer's plot flag
// says; the same goes for the Defpoints layer of an external reference ("xref|Defpoints").
bool isDefpoints(const Document& doc, LayerId id) {
    const Layer* layer = doc.layer(id);
    if (!layer)
        return false;
    std::string name = layer->name;
    for (char& c : name)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    const std::size_t bar = name.find_last_of("|$");
    if (bar != std::string::npos)
        name = name.substr(bar + 1);
    return name == "defpoints";
}

bool plottable(const Document& doc, const Entity& e) {
    if (!e.common.visible || isDefpoints(doc, e.common.layer))
        return false;
    EffectiveLayer l = doc.effective(e.common.layer);
    // Plots show every level that is switched on, whatever the on-screen view of other levels.
    const Level* level = doc.level(e.common.level);
    return l.visible() && l.plot && (!level || level->visible);
}

BBox2 extentsOf(const Document& doc) {
    BBox2 box;
    for (EntityId id : doc.modelSpace()) {
        const Entity* e = doc.find(id);
        if (!e || !plottable(doc, *e))
            continue;
        // A picture counts too: it is plotted (its frame is the size of the picture).
        const InsertData* ins = entityAs<InsertData>(*e);
        const BlockDef* def = ins ? doc.block(ins->block) : nullptr;
        const bool picture = def && !def->xrefPath.empty() && underlay::isImagePath(def->xrefPath);
        if (picture || geom::countsForExtents(*e))
            box.merge(geom::bounds(*e, doc));
    }
    return box;
}

Error invalid(std::string msg) {
    return Error{ErrorCode::InvalidArgument, std::move(msg)};
}

// Dash pattern (+ dash, - gap, 0 dot; drawing units) to alternating on/off paper lengths. A dot
// takes no length on screen; here it is a short dash of `dotMm` taken out of the gap after it, so
// the pattern keeps the period (and the phase) it has on screen.
std::vector<double> dashArray(const std::vector<double>& pattern, double scale, double dotMm) {
    std::vector<double> out;
    bool lastOn = false;  // true when the previous element was an "on" element
    double owed = 0;      // dot length still to be taken from the gap(s) that follow
    for (double v : pattern) {
        if (v < 0) {
            if (!lastOn)
                out.push_back(0);
            const double len = -v * scale;
            const double take = std::min(owed, len);
            owed -= take;
            out.push_back(len - take);
            lastOn = false;
        } else {
            if (lastOn)
                out.push_back(0);
            if (v == 0)
                owed += dotMm;
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
    // Colour trace (PlotSettings::traceColors): drawn colour -> [plotted colour, uses].
    mutable std::map<std::uint32_t, std::pair<Rgba, std::size_t>> seen{};

    Rgba color(Rgba c) const {
        const Rgba out = colorImpl(c);
        if (s.traceColors) {
            auto& row = seen[(std::uint32_t{c.r} << 24) | (std::uint32_t{c.g} << 16) |
                             (std::uint32_t{c.b} << 8) | c.a];
            row.first = out;
            ++row.second;
        }
        return out;
    }

    std::vector<std::string> traceLines() const {
        std::vector<std::string> lines;
        char buf[200];
        for (const auto& [key, row] : seen) {
            const Rgba c{static_cast<std::uint8_t>(key >> 24), static_cast<std::uint8_t>(key >> 16),
                         static_cast<std::uint8_t>(key >> 8), static_cast<std::uint8_t>(key)};
            const int aci = plotstyle::paletteIndexOf({c.r, c.g, c.b, 255});
            std::string entry = "no table";
            if (s.styleTable && aci) {
                const auto& e = s.styleTable->forIndex(aci);
                char eb[96];
                std::snprintf(eb, sizeof eb, "pen %s screen %d%s",
                              e.color ? "set" : "object colour", e.screen,
                              e.grayscale ? " grey" : "");
                entry = eb;
            }
            std::snprintf(buf, sizeof buf,
                          "plot colour: drawn %d,%d,%d alpha %d (ACI %d, %s) -> %d,%d,%d, %zu uses",
                          c.r, c.g, c.b, c.a, aci, entry.c_str(), row.first.r, row.first.g,
                          row.first.b, row.second);
            lines.emplace_back(buf);
        }
        return lines;
    }

    // Plotted colour: the style table's entry for palette colours, then the global colour mode.
    Rgba colorImpl(Rgba c) const {
        // Translucent colours (transparency, dense hatch tones) are mixed with the white sheet
        // after the pen of the style table has been applied: a black pen at 50 % prints grey.
        if (c.a < 255 && !isBackgroundColor(c) && s.styleTable) {
            const double k = c.a / 255.0;
            Rgba opaque = c;
            opaque.a = 255;
            const Rgba pen = colorImpl(opaque);
            auto mix = [k](std::uint8_t v) {
                return static_cast<std::uint8_t>(std::lround(v * k + 255.0 * (1.0 - k)));
            };
            return {mix(pen.r), mix(pen.g), mix(pen.b), 255};
        }
        if (s.styleTable) {
            // True colours use the entry of the nearest palette colour, so monochrome.ctb also
            // blackens them. An entry that keeps the object colour leaves them as they are.
            if (int aci = plotstyle::penIndexOfTrueColor(c)) {
                const plotstyle::PlotStyleEntry& e = s.styleTable->forIndex(aci);
                Rgba shaded = plotstyle::applyEntry(e, plotColor(c, ColorMode::AsDisplayed));
                if (s.color == ColorMode::AsDisplayed)
                    return shaded;
                return plotColor(shaded, s.color);
            }
        }
        return plotColor(c, s.color);
    }
    double weight(Rgba c, double objectMm) const {
        return s.styleTable ? plotstyle::mapLineweight(*s.styleTable, c, objectMm) : objectMm;
    }

    PlotStyle style(const DisplayStyle& ds) const {
        PlotStyle p;
        p.color = color(ds.color);
        const double mm = weight(ds.color, ds.lineweight);
        p.widthMm = (!s.lineweights || !(mm > 0)) ? s.hairlineMm : mm * s.lineweightScale;
        if (ds.pattern && !ds.pattern->empty())
            p.dashMm = dashArray(*ds.pattern, ds.patternScale * layout.paperPerUnit, p.widthMm);
        return p;
    }
    Vec2 pt(Vec2 p) const { return layout.toPaper(p); }

    // `layer` stamps the primitives made from this item (see PlotStyle::layer).
    void item(const DisplayItem& it, std::vector<PlotPrimitive>& out,
              std::uint32_t layer = 0) const {
        if (const auto* pl = std::get_if<DisplayPolyline>(&it)) {
            if (pl->pts.size() < 2)
                return;
            PlotPath path;
            path.closed = pl->closed;
            path.style = style(pl->style);
            path.style.layer = layer;
            path.pts.reserve(pl->pts.size());
            for (const Vec2& p : pl->pts)
                path.pts.push_back(pt(p));
            out.emplace_back(std::move(path));
        } else if (const auto* f = std::get_if<DisplayFill>(&it)) {
            PlotFill fill;
            fill.layer = layer;
            fill.color = color(f->style.color);
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
            dots.style.layer = layer;
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
    if (isBackgroundColor(c))
        return {255, 255, 255, 255};  // a wipeout hides what is behind it on the white sheet
    if (c.a < 255) {  // translucent tones (dense hatches) are mixed with the white sheet
        double k = c.a / 255.0;
        auto mix = [k](std::uint8_t v) {
            return static_cast<std::uint8_t>(std::lround(v * k + 255.0 * (1.0 - k)));
        };
        c = {mix(c.r), mix(c.g), mix(c.b), 255};
    }
    double lum = (0.2126 * c.r + 0.7152 * c.g + 0.0722 * c.b) / 255.0;
    Rgba base = c;
    base.a = 255;
    if (mode == ColorMode::Monochrome)
        return {0, 0, 0, 255};
    if (lum > 0.9)  // white lines would vanish on the white sheet
        return {0, 0, 0, 255};
    // AutoCAD's sheets print index colour 253 as grey 186, not the screen's 130 (measured in the
    // PDF it makes of the S2312 sheet, where the column fills have layer colour 253).
    if (base == paletteColor(253))
        base = {186, 186, 186, 255};
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
        return invalid(s.area == PlotArea::Extents ? "Nothing to plot"
                                                   : "The plot window is empty");
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

namespace {

// Cuts the picture to its clip rectangle (whole pixels) and maps its corners to the sheet.
std::optional<PlotImage> makePlotImage(const underlay::ImageInstance& im, const PlotLayout& lay) {
    const ImageData& src = *im.data;
    const int w = src.width, h = src.height;
    if (w <= 0 || h <= 0 || src.rgba.size() < static_cast<std::size_t>(w) * h * 4)
        return std::nullopt;
    const int px0 = std::clamp(static_cast<int>(std::lround(im.uMin * w)), 0, w - 1);
    const int px1 = std::clamp(static_cast<int>(std::lround(im.uMax * w)), px0 + 1, w);
    const int py0 =
        std::clamp(static_cast<int>(std::lround(im.vMin * h)), 0, h - 1);  // from the top
    const int py1 = std::clamp(static_cast<int>(std::lround(im.vMax * h)), py0 + 1, h);
    PlotImage out;
    out.opacity = im.opacity;
    if (px0 == 0 && px1 == w && py0 == 0 && py1 == h) {
        out.data = im.data;
    } else {
        auto cut = std::make_shared<ImageData>();
        cut->width = px1 - px0;
        cut->height = py1 - py0;
        cut->rgba.resize(static_cast<std::size_t>(cut->width) * cut->height * 4);
        for (int y = 0; y < cut->height; ++y)
            std::copy_n(src.rgba.begin() + (static_cast<std::size_t>(py0 + y) * w + px0) * 4,
                        static_cast<std::size_t>(cut->width) * 4,
                        cut->rgba.begin() + static_cast<std::size_t>(y) * cut->width * 4);
        out.data = std::move(cut);
    }
    // The corners of the instance are for the (unrounded) fractions; move them to the rounded cut.
    const double du = im.uMax - im.uMin, dv = im.vMax - im.vMin;
    if (!(du > 0) || !(dv > 0))
        return std::nullopt;
    auto at = [&](double u, double v) {  // u from the left, v from the top (picture fractions)
        const double s = (u - im.uMin) / du, t = 1.0 - (v - im.vMin) / dv;
        const Vec2 bl = im.corners[0], br = im.corners[1], tl = im.corners[3];
        return lay.toPaper(bl + (br - bl) * s + (tl - bl) * t);
    };
    const double fu0 = static_cast<double>(px0) / w, fu1 = static_cast<double>(px1) / w;
    const double fv0 = static_cast<double>(py0) / h, fv1 = static_cast<double>(py1) / h;
    out.corners = {at(fu0, fv1), at(fu1, fv1), at(fu1, fv0), at(fu0, fv0)};
    return out;
}

// Removes the primitives from `from` on whose box lies wholly outside `window` (paper mm).
void dropOutside(std::vector<PlotPrimitive>& prims, std::size_t from, const BBox2& window) {
    auto boxOf = [](const PlotPrimitive& p) {
        BBox2 b;
        if (const auto* path = std::get_if<PlotPath>(&p)) {
            for (const Vec2& v : path->pts)
                b.expand(v);
            return b.inflated(path->style.widthMm);
        }
        if (const auto* fill = std::get_if<PlotFill>(&p)) {
            for (const auto& ring : fill->rings)
                for (const Vec2& v : ring)
                    b.expand(v);
            return b;
        }
        if (const auto* dots = std::get_if<PlotDots>(&p)) {
            for (const Vec2& v : dots->pts)
                b.expand(v);
            return b.inflated(dots->style.widthMm);
        }
        return b;  // empty: kept
    };
    std::size_t out = from;
    for (std::size_t i = from; i < prims.size(); ++i) {
        const BBox2 b = boxOf(prims[i]);
        if (!b.empty() && !b.intersects(window))
            continue;
        // A drawn primitive with nothing to draw is dropped, so it adds no layer either.
        const bool drawn = std::holds_alternative<PlotPath>(prims[i]) ||
                           std::holds_alternative<PlotFill>(prims[i]) ||
                           std::holds_alternative<PlotDots>(prims[i]);
        if (drawn && b.empty())
            continue;
        if (out != i)
            prims[out] = std::move(prims[i]);
        ++out;
    }
    prims.resize(out);
}

}  // namespace

void appendEntities(PlotSheet& sheet, const Document& doc, const std::vector<EntityId>& ids,
                    const PlotLayout& lay, const PlotSettings& s, const EntityFilter& skip) {
    Builder b{s, lay};

    geom::DisplayContext ctx;
    ctx.chordTolMm = s.chordTolMm / lay.paperPerUnit;
    ctx.annoScale = lay.annoScale;
    ctx.paperSpace = lay.paperSpace;
    // A "pixel" on paper is a hairline: hatches denser than that are drawn as a tone.
    ctx.pixelSizeMm = s.hairlineMm / lay.paperPerUnit;
    ctx.defaultLineweightMm = 0.25;
    ctx.byBlockLineweightMm = 0.25;
    ctx.showTransparency = s.transparency;
    ctx.fillMode = fillMode(doc.header());
    ctx.quickText = quickText(doc.header());
    ctx.attDisplay = attDisplay(doc.header());
    // Slight growth so lines on the border are not culled away.
    const BBox2 cull = lay.area.inflated(1.0 / lay.paperPerUnit);

    // Pictures go first so they lie under the drawing; the frame polyline of a picture is never
    // plotted.
    std::unordered_set<std::uint32_t> pictures;
    {
        underlay::ImageCache cache;
        underlay::ImageParamTable params;
        bool haveParams = false;
        auto resolve = [&](std::string_view stored) {
            std::filesystem::path p(stored);
            if (p.is_relative() && !s.hostFile.empty())
                p = s.hostFile.parent_path() / p;
            return p;
        };
        for (EntityId id : ids) {
            const Entity* e = doc.find(id);
            const InsertData* ins = e ? entityAs<InsertData>(*e) : nullptr;
            const BlockDef* def = ins ? doc.block(ins->block) : nullptr;
            if (!def || def->xrefPath.empty() || !underlay::isImagePath(def->xrefPath))
                continue;
            pictures.insert(id.value);
            if (!plottable(doc, *e) || (skip && skip(*e)))
                continue;
            if (const Layer* xl = doc.layerByName(def->name);
                xl && !doc.effective(xl->id).visible())
                continue;
            if (!haveParams) {
                params = underlay::readImageParams(doc.header());
                haveParams = true;
            }
            auto im = underlay::imageOfInsert(doc, *e, params, cache, resolve);
            if (!im)
                continue;
            if (auto pi = makePlotImage(*im, lay)) {
                if (s.color != ColorMode::AsDisplayed) {  // pictures print in grey, not in colour
                    auto grey = std::make_shared<ImageData>(*pi->data);
                    for (std::size_t i = 0; i + 3 < grey->rgba.size(); i += 4) {
                        const double l = 0.2126 * grey->rgba[i] + 0.7152 * grey->rgba[i + 1] +
                                         0.0722 * grey->rgba[i + 2];
                        grey->rgba[i] = grey->rgba[i + 1] = grey->rgba[i + 2] =
                            static_cast<std::uint8_t>(std::lround(l));
                    }
                    pi->data = std::move(grey);
                }
                ++sheet.entityCount;
                sheet.prims.emplace_back(std::move(*pi));
            }
        }
    }

    // Layer numbers for the PDF layer list; bound external reference layers ("name$0$layer") are
    // written "name|layer".
    // The layers inside an external reference are listed as "xrefname|layer", as AutoCAD names
    // them while the file is attached.
    std::map<std::pair<std::uint64_t, const BlockDef*>, std::uint32_t> layerNumbers;
    auto layerOf = [&](LayerId li, const BlockDef* xref = nullptr) -> std::uint32_t {
        if (auto it = layerNumbers.find({li.value, xref}); it != layerNumbers.end())
            return it->second;
        std::uint32_t n = 0;
        if (const Layer* layer = doc.layer(li)) {
            // An external reference is a layer with the file's layers below it: "xref|layer",
            // nested ones keep the inner "$0$". A bound one is a plain "xref$0$layer".
            std::string name = layer->name;
            if (xref && !layer->parent && name.find('|') == std::string::npos) {
                if (name != xref->name)
                    name = xref->name + "|" + name;
            } else if (layer->parent) {
                for (LayerId up = layer->parent; up;) {
                    const Layer* parent = doc.layer(up);
                    if (!parent)
                        break;
                    name = parent->name + "|" + name;
                    up = parent->parent;
                }
            } else {
                for (std::size_t at; (at = name.find("$0$")) != std::string::npos;)
                    name.replace(at, 3, "|");
            }
            auto found = std::find(sheet.layerNames.begin(), sheet.layerNames.end(), name);
            if (found == sheet.layerNames.end()) {
                sheet.layerNames.push_back(std::move(name));
                found = sheet.layerNames.end() - 1;
            }
            n = static_cast<std::uint32_t>(found - sheet.layerNames.begin()) + 1;
        }
        layerNumbers[{li.value, xref}] = n;
        return n;
    };
    std::unordered_map<std::string, bool> namedLayers;
    auto layerNamed = [&](const std::string& name) {
        auto it = namedLayers.find(name);
        if (it == namedLayers.end()) {
            bool found = false;
            for (const auto& [id, layer] : doc.layers())
                if (layer.name == name) {
                    found = true;
                    break;
                }
            it = namedLayers.emplace(name, found).first;
        }
        return it->second;
    };
    struct LayerTrace {
        std::size_t uses = 0;
        std::vector<std::string> samples;
    };
    std::map<std::uint32_t, LayerTrace> layerTrace;
    BBox2 paperWindow;
    paperWindow.expand(lay.toPaper(lay.area.min));
    paperWindow.expand(lay.toPaper(lay.area.max));
    paperWindow = paperWindow.inflated(1.0);
    for (EntityId id : ids) {
        const Entity* e = doc.find(id);
        if (!e || !plottable(doc, *e) || (skip && skip(*e)))
            continue;
        if (pictures.count(id.value))
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
            arc.style.layer = layerOf(e->common.layer);
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
        const InsertData* ins = entityAs<InsertData>(*e);
        const BlockDef* insBlock = ins ? doc.block(ins->block) : nullptr;
        // An attached external reference, or one bound into the drawing (its name stays behind as a
        // layer), names the layers of its parts "block|layer".
        const BlockDef* xrefBlock =
            insBlock && (!insBlock->xrefPath.empty() || layerNamed(insBlock->name)) ? insBlock
                                                                                    : nullptr;
        for (const DisplayItem& it : list.items) {
            // The parts of a block carry their own layer: off, frozen or frozen in this viewport.
            LayerId li{};
            if (const auto* pl = std::get_if<DisplayPolyline>(&it))
                li = pl->style.layer;
            else if (const auto* fl = std::get_if<DisplayFill>(&it))
                li = fl->style.layer;
            if (li != LayerId{} && li != e->common.layer &&
                (isDefpoints(doc, li) || !doc.effective(li).visible() ||
                 (lay.layerHidden && lay.layerHidden(li))))
                continue;
            const std::size_t before = sheet.prims.size();
            b.item(it, sheet.prims, 0);
            // What lies wholly outside the window is not drawn at all, so it adds no layer.
            dropOutside(sheet.prims, before, paperWindow);
            if (sheet.prims.size() == before)
                continue;
            const std::uint32_t layer = li != LayerId{} && li != e->common.layer
                                            ? layerOf(li, xrefBlock)
                                            : layerOf(e->common.layer);
            if (s.traceColors) {
                auto& tr = layerTrace[layer];
                ++tr.uses;
                if (tr.samples.size() < 3) {
                    BBox2 pb;
                    for (std::size_t k = before; k < sheet.prims.size(); ++k)
                        if (const auto* path = std::get_if<PlotPath>(&sheet.prims[k]))
                            for (const Vec2& v : path->pts)
                                pb.expand(v);
                    const Layer* el = doc.layer(e->common.layer);
                    const Layer* pl = doc.layer(li);
                    const std::string xrefText =
                        xrefBlock ? " xref=" + xrefBlock->xrefPath : std::string();
                    char buf[768];
                    std::snprintf(
                        buf, sizeof buf,
                        "  <- entity %llu kind %d on '%s'%s, part layer '%s', block '%s'%s, "
                        "part at paper (%.1f,%.1f)-(%.1f,%.1f), window (%.1f,%.1f)-(%.1f,%.1f)",
                        static_cast<unsigned long long>(id.value), static_cast<int>(kind(*e)),
                        el ? el->name.c_str() : "?", ins ? " insert" : "",
                        pl ? pl->name.c_str() : "-", insBlock ? insBlock->name.c_str() : "-",
                        xrefText.c_str(), pb.min.x, pb.min.y, pb.max.x, pb.max.y, paperWindow.min.x,
                        paperWindow.min.y, paperWindow.max.x, paperWindow.max.y);
                    tr.samples.emplace_back(buf);
                }
            }
            for (std::size_t k = before; k < sheet.prims.size(); ++k) {
                if (auto* path = std::get_if<PlotPath>(&sheet.prims[k]))
                    path->style.layer = layer;
                else if (auto* fill = std::get_if<PlotFill>(&sheet.prims[k]))
                    fill->layer = layer;
                else if (auto* dots = std::get_if<PlotDots>(&sheet.prims[k]))
                    dots->style.layer = layer;
            }
        }
    }
    if (s.traceColors) {
        for (const auto& [n, tr] : layerTrace) {
            if (n == 0 || n > sheet.layerNames.size())
                continue;
            std::string line =
                "layer '" + sheet.layerNames[n - 1] + "': " + std::to_string(tr.uses) + " parts";
            for (const std::string& sample : tr.samples)
                line += sample;
            sheet.trace.push_back(std::move(line));
        }
    }
    if (s.traceColors)
        for (std::string& line : b.traceLines())
            sheet.trace.push_back(std::move(line));
}

Result<PlotSheet> buildSheet(const Document& doc, const PlotSettings& s) {
    auto layout = computeLayout(doc, s);
    if (!layout)
        return layout.error();
    PlotSheet sheet;
    sheet.layout = layout.value();
    // A window (or a scale that overflows the page) must not let objects spill outside it.
    const PlotLayout& lay = sheet.layout;
    BBox2 clip;
    clip.expand(lay.toPaper(lay.area.min));
    clip.expand(lay.toPaper(lay.area.max));
    const bool clipped = s.area == PlotArea::Window || lay.overflows;
    if (clipped) {
        BBox2 r;
        r.min = {std::max(clip.min.x, lay.printable.min.x),
                 std::max(clip.min.y, lay.printable.min.y)};
        r.max = {std::min(clip.max.x, lay.printable.max.x),
                 std::min(clip.max.y, lay.printable.max.y)};
        if (!r.empty())
            clip = r;
        sheet.prims.emplace_back(PlotClip{clip, true});
    }
    appendEntities(sheet, doc, doc.modelSpace(), sheet.layout, s, {});
    if (clipped)
        sheet.prims.emplace_back(PlotClip{clip, false});
    return sheet;
}

}  // namespace tbcad::exportlib
