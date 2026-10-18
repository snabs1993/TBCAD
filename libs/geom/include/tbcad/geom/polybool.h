// Boolean operations on 2D regions made of polygon rings (union, intersection, difference).
// Shared by REGION / UNION / SUBTRACT / INTERSECT and by anything else that has to merge outlines.
//
// A Region is a set of closed rings read with the even-odd rule, so a ring inside a ring is a hole.
// Results are normalised: outer boundaries run counter-clockwise, holes clockwise, no repeated
// points, no collinear middle vertices.
#pragma once

#include <vector>

#include "tbcad/core/core.h"

namespace tbcad::geom {

using Ring = std::vector<Vec2>;

struct Region {
    std::vector<Ring> rings{};
    bool empty() const { return rings.empty(); }
};

enum class BoolOp { Union, Intersect, Subtract };

// Orients the rings by nesting depth (outer counter-clockwise, holes clockwise) and drops rings
// with fewer than three distinct points or no area.
Region normalized(Region region, double eps = 1e-9);
// Area with holes subtracted.
double area(const Region& region);
// Even-odd point test; points within eps of a boundary count as inside.
bool contains(const Region& region, Vec2 p, double eps = 1e-9);

// a OP b. `eps` is the distance below which points are the same point.
Region boolean(const Region& a, const Region& b, BoolOp op, double eps = 1e-7);

// A ring read back as lines and arcs: runs of vertices that lie on one circle (within `tol`, with a
// turn of at most 25 degrees at every vertex, at least four vertices) become one arc. Used to give
// the results of boolean operations back the arcs that were flattened going in.
struct ArcVertex {
    Vec2 p{};
    double bulge = 0;  // of the segment from this vertex to the next
};
struct ArcFit {
    std::vector<ArcVertex> vertices{};  // empty when the whole ring is one circle
    bool circle = false;
    Vec2 center{};
    double radius = 0;
};
ArcFit fitArcs(const Ring& ring, double tol);

}  // namespace tbcad::geom
