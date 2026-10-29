#pragma once

#include <optional>
#include <vector>

#include "tbcad/core/core.h"

namespace tbcad::geom {

// Revision clouds: polylines of arcs laid along an outline. REVCLOUD marks the polylines it makes
// with XDATA of this application (arc length, the longest chord when it was made, and flags), so
// the cloud can be found again: its grips are the outline's corners, dragging one lays the arcs
// again, and its arc length can be changed. Older clouds without the mark are found by their
// arcs (every one sweeping 120 degrees, all one way).
inline constexpr const char* kCloudApp = "TBCAD_REVCLOUD";

// More arcs than this would make an object too heavy to draw.
inline constexpr std::size_t kMaxCloudArcs = 20000;

struct CloudShape {
    // The corners the arcs are laid along: the cloud's vertices without those that sit on a
    // straight run between two others. The first vertex of the cloud is always the first corner.
    std::vector<Vec2> outline{};
    bool closed = true;
    // Corners stay cloud vertices and every edge is divided on its own. Otherwise (a cloud made
    // from a circle, ellipse or spline) the outline is divided into equal pieces by length.
    bool corners = true;
    double arcLength = 0;  // the longest chord an arc may have
    bool inward = false;   // arcs bulge into the area instead of out of it
    bool calligraphy = false;
};

// A closed cloud along `ring` (an open one along `path` for cloudOpen). The arcs bulge out of the
// area the points enclose, into it with `inward`.
PolylineData cloudClosed(const std::vector<Vec2>& ring, bool corners, double arcLength, bool inward,
                         bool calligraphy);
PolylineData cloudOpen(const std::vector<Vec2>& path, double arcLength, bool inward,
                       bool calligraphy);

// How many arcs a cloud along these points would have.
double cloudArcCount(const std::vector<Vec2>& points, bool closed, bool corners, double arcLength);

// Marks `e` (a cloud polyline) as a revision cloud of the given arc length.
void markCloud(Entity& e, double arcLength, bool corners);

// The cloud `e` is, or nothing when it is not one (any more).
std::optional<CloudShape> cloudShape(const Entity& e);

// `e` laid again as the cloud `shape` describes, keeping its properties and other data. Fails
// when the shape has too few points or would need more than kMaxCloudArcs arcs.
Result<Entity> reshapeCloud(const Entity& e, const CloudShape& shape);

// The grips of a cloud: the outline's corners, then the middle of each outline edge.
std::vector<Vec2> cloudGrips(const CloudShape& shape);
// `e` after dragging cloud grip `index` (as cloudGrips) to `to`: a corner moves, an edge moves
// with both its corners; the arcs are laid again.
Result<Entity> moveCloudGrip(const Entity& e, const CloudShape& shape, int index, Vec2 to);

}  // namespace tbcad::geom
