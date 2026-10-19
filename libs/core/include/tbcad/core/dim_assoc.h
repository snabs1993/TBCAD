#pragma once

#include <optional>

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

// Called after a linked point moved, to bring the measurement and text position up to date. The
// dimension library installs it.
using DimRefreshFn = void (*)(const Document&, Entity&);
void setDimRefresh(DimRefreshFn fn);
DimRefreshFn dimRefresh();

}  // namespace tbcad
