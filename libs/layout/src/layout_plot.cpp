#include <algorithm>
#include <cstdlib>
#include <future>
#include <thread>

#include "tbcad/geom/geom.h"
#include "tbcad/layout/layout.h"
#include "tbcad/text/text.h"

namespace tbcad::layout {

namespace {

// The colour a layer gives its border; white (index 7) prints dark, as everywhere else.
Rgba borderColour(const Color& c) {
    if (c.kind == ColorKind::Rgb)
        return Rgba{c.r, c.g, c.b, 255};
    if (c.kind == ColorKind::Index && c.indexValue() != 7)
        return paletteColor(c.indexValue());
    return Rgba{0, 0, 0, 255};
}

}  // namespace

namespace {

std::size_t lanes() {
    if (std::getenv("SERIAL_X"))
        return 1;
    return std::clamp<std::size_t>(std::thread::hardware_concurrency(), 1, 16);
}

// Runs f(from, to) over [0, n) in slices on several threads; f(0, n) alone when n is small.
template <class F>
void inSlices(std::size_t n, std::size_t minSlice, F&& f) {
    const std::size_t parts = std::min(lanes(), std::max<std::size_t>(1, n / minSlice));
    if (parts < 2) {
        f(std::size_t{0}, n);
        return;
    }
    std::vector<std::future<void>> jobs;
    auto run = [&](std::size_t a, std::size_t b) {
        text::FontScope fonts;  // texts are laid out without waiting for the other threads
        f(a, b);
    };
    for (std::size_t k = 1; k < parts; ++k)
        jobs.push_back(std::async(std::launch::async, run, n * k / parts, n * (k + 1) / parts));
    run(0, n / parts);
    for (auto& j : jobs)
        j.get();
}

// exportlib::appendEntities over many objects, on several threads. The pieces are joined in
// order: pictures first (they lie under the drawing), then the rest, with the layer numbers of
// each piece renumbered into the sheet's list.
void appendMany(exportlib::PlotSheet& sheet, const Document& doc, const std::vector<EntityId>& ids,
                const exportlib::PlotLayout& lay, const exportlib::PlotSettings& s,
                const exportlib::EntityFilter& skip) {
    constexpr std::size_t kSlice = 8192;
    if (ids.size() < 2 * kSlice || lanes() < 2 || s.traceColors) {
        exportlib::appendEntities(sheet, doc, ids, lay, s, skip);
        return;
    }
    const std::size_t parts = std::min(lanes(), ids.size() / kSlice);
    std::vector<exportlib::PlotSheet> out(parts);
    inSlices(parts, 1, [&](std::size_t a, std::size_t b) {
        for (std::size_t k = a; k < b; ++k) {
            const auto from = ids.begin() + static_cast<std::ptrdiff_t>(ids.size() * k / parts);
            const auto to = ids.begin() + static_cast<std::ptrdiff_t>(ids.size() * (k + 1) / parts);
            out[k].layout = sheet.layout;
            exportlib::appendEntities(out[k], doc, std::vector<EntityId>(from, to), lay, s, skip);
        }
    });
    // Layer numbers of each piece in the sheet's list.
    std::vector<std::vector<std::uint32_t>> renumber(parts);
    bool pictures = false;
    for (std::size_t k = 0; k < parts; ++k) {
        const exportlib::PlotSheet& piece = out[k];
        renumber[k].assign(piece.layerNames.size() + 1, 0);
        for (std::size_t i = 0; i < piece.layerNames.size(); ++i) {
            auto found =
                std::find(sheet.layerNames.begin(), sheet.layerNames.end(), piece.layerNames[i]);
            if (found == sheet.layerNames.end()) {
                sheet.layerNames.push_back(piece.layerNames[i]);
                found = sheet.layerNames.end() - 1;
            }
            renumber[k][i + 1] = static_cast<std::uint32_t>(found - sheet.layerNames.begin()) + 1;
        }
        sheet.entityCount += piece.entityCount;
        for (const exportlib::PlotPrimitive& prim : piece.prims)
            if (std::holds_alternative<exportlib::PlotImage>(prim)) {
                pictures = true;
                break;
            }
    }
    auto moveIn = [&](std::size_t k, exportlib::PlotPrimitive& prim, exportlib::PlotPrimitive& to) {
        auto fix = [&](std::uint32_t& layer) {
            if (layer < renumber[k].size())
                layer = renumber[k][layer];
        };
        if (auto* path = std::get_if<exportlib::PlotPath>(&prim))
            fix(path->style.layer);
        else if (auto* arc = std::get_if<exportlib::PlotArc>(&prim))
            fix(arc->style.layer);
        else if (auto* fill = std::get_if<exportlib::PlotFill>(&prim))
            fix(fill->layer);
        else if (auto* dots = std::get_if<exportlib::PlotDots>(&prim))
            fix(dots->style.layer);
        to = std::move(prim);
    };
    if (pictures) {
        for (const bool picture : {true, false})
            for (std::size_t k = 0; k < parts; ++k)
                for (exportlib::PlotPrimitive& prim : out[k].prims)
                    if (std::holds_alternative<exportlib::PlotImage>(prim) == picture)
                        moveIn(k, prim, sheet.prims.emplace_back());
        return;
    }
    // Each piece moves into its own stretch of the sheet, on its own thread.
    std::vector<std::size_t> at(parts + 1, sheet.prims.size());
    for (std::size_t k = 0; k < parts; ++k)
        at[k + 1] = at[k] + out[k].prims.size();
    sheet.prims.resize(at[parts]);
    inSlices(parts, 1, [&](std::size_t a, std::size_t b) {
        for (std::size_t k = a; k < b; ++k) {
            for (std::size_t i = 0; i < out[k].prims.size(); ++i)
                moveIn(k, out[k].prims[i], sheet.prims[at[k] + i]);
            exportlib::PlotSheet{}.prims.swap(out[k].prims);  // freed on this thread
        }
    });
}

}  // namespace

Result<exportlib::PlotSheet> buildLayoutSheet(const Document& doc, const std::string& layoutName,
                                              const exportlib::PlotSettings& base,
                                              const LayoutPlotOptions& options) {
    auto found = findLayout(doc, layoutName);
    if (!found)
        return Error{ErrorCode::NotFound, "No layout named \"" + layoutName + "\""};
    const Layout& l = *found;

    // The sheet itself is plotted 1:1 with no margins: sheet millimetres are paper millimetres.
    exportlib::PlotSettings s = base;
    s.paperName = "Custom";
    s.customWidthMm = l.widthMm;
    s.customHeightMm = l.heightMm;
    s.landscape = l.widthMm > l.heightMm;
    s.marginMm = 0;
    s.area = exportlib::PlotArea::Window;
    s.window = {};
    s.window.expand({0, 0});
    s.window.expand({l.widthMm, l.heightMm});
    s.fitToPaper = false;
    s.scaleN = 1;
    s.centerOnPaper = true;
    auto lay = exportlib::computeLayout(doc, s);
    if (!lay)
        return lay.error();

    exportlib::PlotSheet sheet;
    sheet.layout = lay.value();
    if (const BlockDef* block = doc.block(l.content)) {
        exportlib::PlotLayout paperLay = sheet.layout;
        paperLay.paperSpace = true;
        exportlib::appendEntities(sheet, doc, block->entities, paperLay, s);
    }

    const std::vector<EntityId> model = doc.modelSpace();
    // Every viewport only needs the objects that reach its window. The bounds are worked out once
    // here (for the kinds whose size does not depend on the viewport scale) instead of once per
    // viewport inside appendEntities, which matters with a few hundred thousand objects.
    struct Boxed {
        EntityId id;
        BBox2 box;
        bool always;
        bool found = false;
    };
    std::vector<Boxed> boxed;
    if (!l.viewports.empty()) {
        // Several threads read the document: the lookups that fill a cache on first use are done
        // now.
        for (const auto& [id, layer] : doc.layers())
            (void)doc.effective(id);
        boxed.resize(model.size());
        inSlices(model.size(), 8192, [&](std::size_t from, std::size_t to) {
            for (std::size_t i = from; i < to; ++i) {
                const EntityId id = model[i];
                const Entity* e = doc.find(id);
                if (!e) {
                    boxed[i] = Boxed{id, BBox2{}, false};
                    continue;
                }
                bool fixed = false;
                switch (kind(*e)) {
                    case EntityKind::Line:
                    case EntityKind::Circle:
                    case EntityKind::Arc:
                    case EntityKind::Polyline:
                    case EntityKind::Ellipse:
                    case EntityKind::Spline:
                    case EntityKind::Point:
                    case EntityKind::Text:
                    case EntityKind::MText:
                        fixed = !e->common.annotative;
                        break;
                    default:
                        break;
                }
                Boxed b{id, fixed ? geom::bounds(*e, doc) : BBox2{}, !fixed};
                if (!b.always && b.box.empty())
                    b.always = true;
                b.found = true;
                boxed[i] = std::move(b);
            }
        });
    }
    for (const Viewport& vp : l.viewports) {
        if (!vp.on || vp.paper.empty() || !(vp.scaleN > 0))
            continue;
        // The viewport shows the model window of the same size as the rectangle, scaled 1:N.
        exportlib::PlotLayout vl = sheet.layout;
        vl.paperPerUnit = 1.0 / vp.scaleN;
        const Vec2 half{vp.paper.width() * vp.scaleN / 2, vp.paper.height() * vp.scaleN / 2};
        vl.area.min = vp.center - half;
        vl.area.max = vp.center + half;
        vl.origin = vp.paper.min;
        vl.overflows = false;
        vl.annoScale = vp.scaleN;
        vl.layerHidden = [&doc, &vp](LayerId l) { return frozenInViewport(doc, vp, l); };

        sheet.prims.emplace_back(exportlib::PlotClip{vp.paper, true, vp.clip});
        const BBox2 reach = vl.area.inflated(2.0 / vl.paperPerUnit);
        std::vector<EntityId> near;
        for (const Boxed& b : boxed)
            if (b.found && (b.always || b.box.intersects(reach)))
                near.push_back(b.id);
        appendMany(sheet, doc, near, vl, s, [&](const Entity& e) {
            if (e.common.annotative && !e.common.annoScales.empty() &&
                std::find(e.common.annoScales.begin(), e.common.annoScales.end(), vp.scaleN) ==
                    e.common.annoScales.end())
                return true;  // made for other scales
            return frozenInViewport(doc, vp, e.common.layer) ||
                   (vp.level.value != 0 && e.common.level != vp.level);
        });
        sheet.prims.emplace_back(exportlib::PlotClip{vp.paper, false, {}});

        // The border goes with its layer: off, frozen or not plotting leaves the sheet without it.
        const Layer* borderLayer = vp.borderLayer.value ? doc.layer(vp.borderLayer) : nullptr;
        const bool layerShows =
            !borderLayer || (borderLayer->on && !borderLayer->frozen && borderLayer->plot);
        if (options.viewportBorders && vp.border && layerShows) {
            exportlib::PlotPath border;
            border.closed = true;
            border.style.color = exportlib::plotColor(
                borderLayer ? borderColour(borderLayer->color) : Rgba{0, 0, 0, 255}, s.color);
            border.style.widthMm = s.lineweights ? 0.25 : s.hairlineMm;
            border.pts = viewportOutline(vp);
            sheet.prims.emplace_back(std::move(border));
        }
    }
    return sheet;
}

}  // namespace tbcad::layout
