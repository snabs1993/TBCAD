#include <map>
#include <sstream>
#include <string>

#include "tbcad/export/plot.h"
#include "tbcad/pdf/pdf.h"

namespace tbcad::exportlib {

namespace {

bool sameStyle(const PlotStyle& a, const PlotStyle& b) {
    return a.color == b.color && a.widthMm == b.widthMm && a.dashMm == b.dashMm;
}

class PdfPainter {
public:
    // `docLayers[i]` is the PDF layer of the sheet's layer i + 1.
    PdfPainter(pdf::Page& page, const std::vector<int>& docLayers)
        : page_(page), docLayers_(docLayers) {}

    // Marked content for the layer of the next thing drawn; layer 0 closes the open one.
    void layer(std::uint32_t l) {
        if (l == layer_)
            return;
        closeLayer();
        if (l >= 1 && l <= docLayers_.size()) {
            page_.beginLayer(docLayers_[l - 1]);
            layer_ = l;
        }
    }
    void closeLayer() {
        if (layer_ != 0)
            page_.endLayer();
        layer_ = 0;
    }

    void path(const PlotPath& p) {
        layer(p.style.layer);
        apply(p.style, p.style.dashMm.empty() ? pdf::LineCap::Round : pdf::LineCap::Butt);
        page_.moveTo(p.pts[0].x, p.pts[0].y);
        for (std::size_t i = 1; i < p.pts.size(); ++i)
            page_.lineTo(p.pts[i].x, p.pts[i].y);
        if (p.closed)
            page_.closePath();
        page_.stroke();
    }

    void arc(const PlotArc& a) {
        layer(a.style.layer);
        apply(a.style, a.style.dashMm.empty() ? pdf::LineCap::Round : pdf::LineCap::Butt);
        if (a.full) {
            page_.moveTo(a.center.x + a.radius, a.center.y);
            page_.arcTo(a.center.x, a.center.y, a.radius, 0, kTwoPi, true);
            page_.closePath();
        } else {
            page_.arcTo(a.center.x, a.center.y, a.radius, a.start, a.end, true);
        }
        page_.stroke();
    }

    void fill(const PlotFill& f) {
        layer(f.layer);
        page_.setFillRGB(f.color.r / 255.0, f.color.g / 255.0, f.color.b / 255.0);
        for (const auto& ring : f.rings) {
            page_.moveTo(ring[0].x, ring[0].y);
            for (std::size_t i = 1; i < ring.size(); ++i)
                page_.lineTo(ring[i].x, ring[i].y);
            page_.closePath();
        }
        page_.fill(pdf::FillRule::EvenOdd);
    }

    void dots(const PlotDots& d) {
        layer(d.style.layer);
        PlotStyle st = d.style;
        apply(st, pdf::LineCap::Round);
        for (const Vec2& p : d.pts) {
            page_.moveTo(p.x, p.y);
            page_.lineTo(p.x + 1e-3, p.y);
            page_.stroke();
        }
    }

    // Graphics state was saved or restored behind our back: forget what is set.
    void reset() {
        have_ = false;
        cap_ = pdf::LineCap::Butt;
    }

private:
    void apply(const PlotStyle& st, pdf::LineCap cap) {
        if (!have_ || !sameStyle(st, last_)) {
            page_.setStrokeRGB(st.color.r / 255.0, st.color.g / 255.0, st.color.b / 255.0);
            page_.setLineWidthMm(st.widthMm);
            page_.setDashMm(st.dashMm);
            last_ = st;
            have_ = true;
        }
        if (cap != cap_) {
            page_.setLineCap(cap);
            cap_ = cap;
        }
    }

    pdf::Page& page_;
    const std::vector<int>& docLayers_;
    std::uint32_t layer_ = 0;
    PlotStyle last_{};
    bool have_ = false;
    pdf::LineCap cap_ = pdf::LineCap::Butt;
};

void paint(const PlotSheet& sheet, pdf::PdfDocument& doc, std::map<std::string, int>& layerIds) {
    std::vector<int> docLayers;
    for (const std::string& name : sheet.layerNames) {
        auto it = layerIds.find(name);
        if (it == layerIds.end())
            it = layerIds.emplace(name, doc.addLayer(name)).first;
        docLayers.push_back(it->second);
    }
    const PlotLayout& lay = sheet.layout;
    pdf::Page& page = doc.addPage(lay.pageWidthMm, lay.pageHeightMm);
    page.save();
    page.clipRect(lay.printable.min.x, lay.printable.min.y, lay.printable.width(),
                  lay.printable.height());
    page.setLineJoin(pdf::LineJoin::Round);
    PdfPainter painter(page, docLayers);
    for (const PlotPrimitive& prim : sheet.prims) {
        if (const auto* p = std::get_if<PlotPath>(&prim))
            painter.path(*p);
        else if (const auto* a = std::get_if<PlotArc>(&prim))
            painter.arc(*a);
        else if (const auto* f = std::get_if<PlotFill>(&prim))
            painter.fill(*f);
        else if (const auto* d = std::get_if<PlotDots>(&prim))
            painter.dots(*d);
        else if (const auto* im = std::get_if<PlotImage>(&prim)) {
            painter.closeLayer();
            page.drawImage(im->data, im->corners[0].x, im->corners[0].y, im->corners[1].x,
                           im->corners[1].y, im->corners[3].x, im->corners[3].y, im->opacity);
        } else if (const auto* c = std::get_if<PlotClip>(&prim)) {
            painter.closeLayer();
            if (c->begin) {
                page.save();
                if (c->poly.size() >= 3) {
                    std::vector<std::pair<double, double>> pts;
                    for (const Vec2& p : c->poly)
                        pts.emplace_back(p.x, p.y);
                    page.clipPolygon(pts);
                } else {
                    page.clipRect(c->rect.min.x, c->rect.min.y, c->rect.width(), c->rect.height());
                }
                page.setLineJoin(pdf::LineJoin::Round);
                painter.reset();
            } else {
                page.restore();
                painter.reset();
            }
        }
    }
    painter.closeLayer();
    page.restore();
}

}  // namespace

Result<void> writePdf(const PlotSheet& sheet, const std::string& path, const std::string& title,
                      bool compress) {
    return writePdfPages({sheet}, path, title, compress);
}

Result<void> writePdfPages(const std::vector<PlotSheet>& sheets, const std::string& path,
                           const std::string& title, bool compress) {
    pdf::PdfDocument doc;
    doc.setCompression(compress);
    doc.setInfo(title, "TBCAD");
    std::map<std::string, int> layerIds;
    for (const PlotSheet& sheet : sheets)
        paint(sheet, doc, layerIds);
    return doc.writeFile(path);
}

Result<std::string> pdfBytes(const PlotSheet& sheet, bool compress) {
    pdf::PdfDocument doc;
    doc.setCompression(compress);
    doc.setInfo("TBCAD drawing", "TBCAD");
    std::map<std::string, int> layerIds;
    paint(sheet, doc, layerIds);
    std::ostringstream out;
    Result<void> r = doc.write(out);
    if (!r)
        return r.error();
    return out.str();
}

}  // namespace tbcad::exportlib
