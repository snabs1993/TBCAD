#include "tbcad/drafting/viewport_snap.h"

#include <algorithm>

namespace tbcad::drafting {

namespace {
Vec2 paperMiddle(const layout::Viewport& vp) {
    return (vp.paper.min + vp.paper.max) * 0.5;
}
}  // namespace

Vec2 sheetFromModel(const layout::Viewport& vp, Vec2 model) {
    return paperMiddle(vp) + (model - vp.center) / vp.scaleN;
}

Vec2 modelFromSheet(const layout::Viewport& vp, Vec2 sheet) {
    return vp.center + (sheet - paperMiddle(vp)) * vp.scaleN;
}

std::vector<SnapCandidate> findViewportSnapCandidates(const Document& doc,
                                                      const index::SpatialIndex& modelIndex,
                                                      const layout::Layout& layout,
                                                      const SnapRequest& rq,
                                                      const SnapGeometryRegistry& providers) {
    std::vector<SnapCandidate> out;
    for (const layout::Viewport& vp : layout.viewports) {
        if (!vp.on || !(vp.scaleN > 0) || !layout::viewportContains(vp, rq.cursor))
            continue;
        SnapRequest mr = rq;
        mr.cursor = modelFromSheet(vp, rq.cursor);
        mr.apertureMm = rq.apertureMm * vp.scaleN;
        if (rq.base)
            mr.base = modelFromSheet(vp, *rq.base);
        for (SnapCandidate c : findSnapCandidates(doc, modelIndex, mr, providers)) {
            const Entity* e = doc.find(c.entity);
            if (e && layout::frozenInViewport(doc, vp, e->common.layer))
                continue;
            c.point = sheetFromModel(vp, c.point);
            // A snap point outside the viewport's outline is not visible there.
            if (!layout::viewportContains(vp, c.point))
                continue;
            c.distance = distance(c.point, rq.cursor);
            out.push_back(c);
        }
    }
    std::sort(out.begin(), out.end(), [](const SnapCandidate& a, const SnapCandidate& b) {
        return a.distance != b.distance ? a.distance < b.distance : a.mode < b.mode;
    });
    if (out.size() > rq.maxCandidates)
        out.resize(rq.maxCandidates);
    return out;
}

}  // namespace tbcad::drafting
