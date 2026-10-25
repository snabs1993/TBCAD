#include <algorithm>

#include "tbcad/clipboard/clipboard.h"
#include "tbcad/export/plot.h"
#include "tbcad/geom/entity.h"
#include "tbcad/platform/clipboard_picture.h"
#include "tbcad/text/text.h"

namespace tbcad::clipboard {

std::optional<platform::ClipboardPicture> renderPicture(const Document& doc,
                                                        std::span<const EntityId> ids) {
    constexpr std::size_t kMaxObjects = 50000;
    constexpr double kLongSideMm = 160, kDpi = 254;  // 1600 pixels
    if (ids.empty() || ids.size() > kMaxObjects)
        return std::nullopt;
    BBox2 box;
    for (EntityId id : ids)
        if (const Entity* e = doc.find(id)) {
            const BBox2 b = geom::bounds(*e, doc);
            if (!b.empty()) {
                box.expand(b.min);
                box.expand(b.max);
            }
        }
    if (box.empty())
        return std::nullopt;
    exportlib::PlotSettings s;
    s.paperName = "Custom";
    s.area = exportlib::PlotArea::Window;
    s.window = box;
    s.marginMm = 2;
    const double w = std::max(box.width(), 1e-9), h = std::max(box.height(), 1e-9);
    const double longSide = std::max(w, h), shortSide = std::min(w, h);
    // Keep thin drawings (a single line) from becoming a one-pixel strip.
    const double shortMm = std::max(10.0, kLongSideMm * shortSide / longSide);
    s.landscape = w >= h;
    s.customWidthMm = s.landscape ? kLongSideMm : shortMm;
    s.customHeightMm = s.landscape ? shortMm : kLongSideMm;
    auto layout = exportlib::computeLayout(doc, s);
    if (!layout)
        return std::nullopt;
    exportlib::PlotSheet sheet;
    sheet.layout = layout.value();
    exportlib::appendEntities(sheet, doc, std::vector<EntityId>(ids.begin(), ids.end()),
                              sheet.layout, s);
    if (sheet.entityCount == 0)
        return std::nullopt;
    auto img = exportlib::rasterize(sheet, kDpi, 8'000'000);
    if (!img)
        return std::nullopt;
    platform::ClipboardPicture pic;
    pic.width = img.value().width;
    pic.height = img.value().height;
    pic.rgba.resize(std::size_t{4} * pic.width * pic.height);
    const auto& rgb = img.value().rgb;
    for (std::size_t i = 0, n = std::size_t{1} * pic.width * pic.height; i < n; ++i) {
        pic.rgba[4 * i] = rgb[3 * i];
        pic.rgba[4 * i + 1] = rgb[3 * i + 1];
        pic.rgba[4 * i + 2] = rgb[3 * i + 2];
        pic.rgba[4 * i + 3] = 255;
    }
    return pic;
}

std::string plainTextOf(const Document& doc, std::span<const EntityId> ids) {
    std::string out;
    for (EntityId id : ids) {
        const Entity* e = doc.find(id);
        if (!e)
            continue;
        std::string line;
        if (auto* t = entityAs<TextData>(*e))
            line = t->utf8;
        else if (auto* m = entityAs<MTextData>(*e))
            line = text::plainText(m->utf8);
        else
            continue;
        if (!out.empty())
            out += '\n';
        out += line;
    }
    return out;
}

}  // namespace tbcad::clipboard
