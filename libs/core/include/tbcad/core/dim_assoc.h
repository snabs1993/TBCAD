#pragma once

#include <optional>
#include <utility>
#include <vector>

#include "tbcad/core/entities.h"

namespace tbcad {

class Document;

// The point of `e` that the association refers to; empty when the object no longer has it.
std::optional<Vec2> dimAssocPoint(const Entity& e, const DimAssoc& a);

// The association for a definition point `p` of a dimension owned by `owner`: the first object of
// that space that has a vertex or centre exactly there (an object-snapped point), if any.
std::optional<DimAssoc> findDimAssoc(const Document& doc, Vec2 p, BlockId owner);

// The association for a point `p` lying on a circle or arc of that space (by its angle), if any.
std::optional<DimAssoc> findDimAssocOnCurve(const Document& doc, Vec2 p, BlockId owner);

// Links every definition point of `d` that lies on a vertex or centre of an object. Returns how
// many points got linked.
int linkDimension(const Document& doc, DimensionData& d, BlockId owner);

// Trans-spatial dimensions: a dimension on a layout sheet whose points follow model objects seen
// through a viewport (DimAssoc::viewport). The layout library lives above the core, so whoever
// has it installs these two lookups. `owner` is the block the dimension is in (the sheet's block).
struct DimViewMap {
    Vec2 sheetMid{};    // middle of the viewport's rectangle on the sheet
    Vec2 center{};      // the model point shown there
    double scaleN = 1;  // 1 sheet mm = scaleN model units
    Vec2 toSheet(Vec2 model) const { return sheetMid + (model - center) / scaleN; }
    Vec2 toModel(Vec2 sheet) const { return center + (sheet - sheetMid) * scaleN; }
};
using DimViewFn = std::optional<DimViewMap> (*)(const Document&, BlockId owner, int viewport);
// The viewports (id, map) of the sheet that show the sheet point.
using DimViewsAtFn = std::vector<std::pair<int, DimViewMap>> (*)(const Document&, BlockId owner,
                                                                 Vec2 sheetPoint);
void setDimViewLookups(DimViewFn view, DimViewsAtFn viewsAt);
std::optional<DimViewMap> dimViewMap(const Document& doc, BlockId owner, int viewport);

// Sets DimensionData::modelScale from the viewports its trans-spatial links use (0 if none).
void syncModelScale(const Document& doc, DimensionData& d, BlockId owner);

// Called after a linked point moved, to bring the measurement and text position up to date. The
// dimension library installs it.
using DimRefreshFn = void (*)(const Document&, Entity&);
void setDimRefresh(DimRefreshFn fn);
DimRefreshFn dimRefresh();

// Called after a dimension, leader or multileader with automatic DIMBREAK gaps (breakWith), or one
// of the objects that break it, changed: works those gaps out again. The dimension commands
// install it.
void setDimBreakRefresh(DimRefreshFn fn);
DimRefreshFn dimBreakRefresh();

}  // namespace tbcad
