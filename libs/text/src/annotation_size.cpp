#include "tbcad/text/annotation_size.h"

#include <algorithm>
#include <cmath>

#include "tbcad/geom/geom.h"

namespace tbcad::text {

double suggestedAnnotationScale(const Document& doc, double pixelMm, bool onSheet,
                                double paperHeightMm) {
    if (onSheet)
        return 1;
    const double n = currentAnnotationScale(doc.header());
    if (n > 1)
        return n;
    const double base = std::max(paperHeightMm, 1e-6);
    double wanted = 0;
    if (pixelMm > 0 && std::isfinite(pixelMm))
        wanted = 12.0 * pixelMm / base;
    BBox2 box;
    doc.forEachEntity([&](const Entity& e) {
        if (!doc.ownerBlock(e.common.id))
            box.merge(geom::bounds(e, doc));
    });
    if (!box.empty()) {
        const double extent = std::max(box.width(), box.height());
        if (std::isfinite(extent) && extent > 0)
            wanted = std::max(wanted, extent / 160.0 / base);
    }
    if (wanted < 8)
        return 1;
    double best = 1;
    for (double decade = 1; decade <= 1e7; decade *= 10)
        for (double step : {1.0, 2.0, 5.0})
            if (std::fabs(std::log(decade * step / wanted)) < std::fabs(std::log(best / wanted)))
                best = decade * step;
    return best;
}

}  // namespace tbcad::text
