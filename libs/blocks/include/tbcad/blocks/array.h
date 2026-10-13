#pragma once

#include <optional>
#include <vector>

#include "tbcad/core/core.h"

namespace tbcad::blocks {

struct RectArray {
    int rows = 1;
    int columns = 1;
    // Distance between rows (along the rotated Y axis) and columns (along the rotated X axis).
    // Negative values go the other way.
    double rowSpacing = 0;
    double columnSpacing = 0;
    double angle = 0;  // rotation of the whole grid, radians
};

struct PolarArray {
    Vec2 center{};
    int count = 1;  // number of items including the original
    // Angle filled by the items, radians; positive = counter-clockwise. 2*pi spreads the items
    // evenly around the circle, anything else puts the last item at the end of the sweep.
    double fillAngle = kTwoPi;
    bool rotateItems = true;
    // Point that travels around the circle when items are not rotated; default is the centre of
    // the source objects' bounding box.
    std::optional<Vec2> basePoint{};
};

// Copies of `source` for every cell except the first (row 0, column 0), which is the original.
// Result entities have a null id. Row 0 / column 0 is at the original position; copies are made
// row by row.
Result<std::vector<Entity>> arrayRectangular(const std::vector<Entity>& source,
                                             const RectArray& spec);
// Copies for items 1..count-1 (item 0 is the original).
Result<std::vector<Entity>> arrayPolar(const std::vector<Entity>& source, const PolarArray& spec);

// Transforms of the copies, in the order the entities are produced (cell 0 excluded).
std::vector<Transform2> rectangularTransforms(const RectArray& spec);
// `reference` is the point of the source that follows the circle when items are not rotated.
std::vector<Transform2> polarTransforms(const PolarArray& spec, Vec2 reference);

// Associative arrays (InsertData::array): the transforms of every item, in block coordinates and
// including item 0. Rows run row by row. Never more than kMaxArrayItems entries.
std::vector<Transform2> arrayItemTransforms(const ArrayInfo& info);
// Number of items arrayItemTransforms returns.
long long arrayItemCount(const ArrayInfo& info);

// Largest number of copies an array may create (guards against typos like 1e6 x 1e6).
inline constexpr long long kMaxArrayItems = 100000;

}  // namespace tbcad::blocks
