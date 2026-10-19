#pragma once

#include <cstddef>
#include <optional>
#include <string_view>
#include <vector>

#include "tbcad/modify/common.h"

namespace tbcad::modify {

// ---- LENGTHEN -----------------------------------------------------------------------------------

enum class LengthenMode {
    Delta,    // add `value` mm of length (negative shortens)
    Percent,  // new total length = value percent of the current one
    Total,    // new total length = value mm
};

// Lengthens or shortens a line, an arc or an open polyline at its end nearest to `pick`. Shortening
// may remove whole trailing segments of a polyline. Closed curves, circles, ellipses and splines
// cannot be lengthened.
Result<EntityEdit> lengthenEntity(const Entity& e, Vec2 pick, LengthenMode mode, double value);
// Moves the end nearest to `pick` to where `target` falls on the extended line or arc (polylines
// use their last segment).
Result<EntityEdit> lengthenToPoint(const Entity& e, Vec2 pick, Vec2 target);
// Total length of a line, arc or open polyline (nullopt for the other kinds).
std::optional<double> lengthenableLength(const Entity& e);

// ---- REVERSE ------------------------------------------------------------------------------------

// Reverses the direction of a line, polyline or spline. Other entities have no direction.
Result<EntityEdit> reverseEntity(const Entity& e);

// ---- DIVIDE / MEASURE ---------------------------------------------------------------------------

// Points that divide a line, arc, circle, ellipse, spline or polyline into `count` equal parts by
// length: count - 1 points on an open curve, count points on a closed one (the first at the
// start).
Result<std::vector<Vec2>> dividePoints(const Entity& e, int count);
// Points at multiples of `segment` mm from the start (or from the end when `fromEnd`). The start
// itself is not included.
Result<std::vector<Vec2>> measurePoints(const Entity& e, double segment, bool fromEnd = false);

// ---- ALIGN --------------------------------------------------------------------------------------

// Moves src1 onto dst1; with a second pair also rotates so that src1->src2 points along
// dst1->dst2, and optionally scales so that their distances match.
Result<Transform2> alignTransform(Vec2 src1, Vec2 dst1, std::optional<Vec2> src2,
                                  std::optional<Vec2> dst2, bool scale);

// ---- OVERKILL -----------------------------------------------------------------------------------

struct OverkillOptions {
    double tolerance = 1e-6;
    bool ignoreProperties = false;   // compare geometry only
    bool combineOverlapping = true;  // merge collinear lines / same-circle arcs that overlap
    bool combineEndToEnd = true;     // ... and ones that only touch end to end
};

struct OverkillResult {
    std::vector<std::size_t> erased{};  // indices into the input to delete
    std::vector<Entity> created{};      // replacements for merged groups
    std::size_t duplicates = 0;         // entries of `erased` that were exact duplicates
};

// Removes duplicate entities and merges overlapping collinear lines and arcs of one circle.
// Lines, arcs, circles, points, polylines and ellipses take part; other kinds are left alone.
OverkillResult overkill(const std::vector<const Entity*>& entities,
                        const OverkillOptions& options = {});

// ---- PEDIT --------------------------------------------------------------------------------------

// A line or arc as a polyline with the same properties.
Result<Entity> toPolylineEntity(const Entity& e);
// Cubic spline through the vertices of a polyline (arcs are sampled).
Result<Entity> polylineToSpline(const Entity& e);
// PEDIT Fit: each segment becomes two tangent-continuous arcs; the curve passes through every
// vertex. Existing bulges are ignored.
Result<Entity> fitPolylineCurve(const Entity& e);
// Polyline that follows a spline within `tolerance` mm.
Result<Entity> splineToPolyline(const Entity& e, double tolerance);
// Sets the constant width of every segment of a polyline entity.
Result<EntityEdit> setPolylineWidth(const Entity& e, double width);

// ---- EXTRIM -------------------------------------------------------------------------------------

// Trims `target` at its crossings with the single cutting edge and removes every part that lies on
// the same side of the edge as `side`. The edge may be a line (taken as infinite), a circle or an
// arc (taken as the full circle, sides are inside / outside) or a closed polyline. Errors: another
// kind of edge; the target does not cross the edge. When nothing of the target lies on that side
// the result is "no change" (an empty edit).
Result<EntityEdit> extrimEntity(const Entity& target, const Entity& edge, Vec2 side);

// ---- BLEND --------------------------------------------------------------------------------------

// A tangent-continuous spline between the ends of two open curves (line, arc, open polyline or
// open spline), taking the end of each nearest to its pick point. Properties come from `a`.
Result<Entity> blendCurves(const Entity& a, Vec2 pickA, const Entity& b, Vec2 pickB);

// ---- PEDIT vertices / SPLINEDIT ---------------------------------------------------------------

// Inserts a vertex at the point of the polyline closest to `p`, splitting a straight or arc
// segment. Per-vertex widths are not kept.
Result<EntityEdit> insertPolylineVertex(const Entity& e, Vec2 p);
// Removes the vertex nearest to `near`; its neighbours are joined by a straight segment.
Result<EntityEdit> removePolylineVertex(const Entity& e, Vec2 near);
// Moves the vertex nearest to `near` to `to`; arc bulges are kept.
Result<EntityEdit> movePolylineVertex(const Entity& e, Vec2 near, Vec2 to);
// Gives the segment that starts at the vertex nearest to `near` a start and an end width.
Result<EntityEdit> setPolylineVertexWidth(const Entity& e, Vec2 near, double startWidth,
                                          double endWidth);
// Replaces everything between the vertices nearest to `a` and `b` by one straight segment.
Result<EntityEdit> straightenPolyline(const Entity& e, Vec2 a, Vec2 b);
// Widths that change evenly along the whole length, from `startWidth` to `endWidth`.
Result<EntityEdit> taperPolyline(const Entity& e, double startWidth, double endWidth);

enum class SplineFitOp { Add, Delete, Move, Close, Open };
// Edits the fit points of a spline and fits it again. Add: a new fit point on the curve closest to
// `a`; Delete: the fit point nearest to `a`; Move: the fit point nearest to `a` goes to `b`;
// Close / Open change whether the curve returns to its first point. Errors when the spline has
// no fit points or fewer than two would remain.
Result<EntityEdit> editSplineFit(const Entity& e, SplineFitOp op, Vec2 a = {}, Vec2 b = {});

// Fits the spline again through as few points as it takes: the new curve interpolates fit points
// taken from the old one and stays within `tolerance` of it (SPLINEDIT Fit data / Tolerance). The
// old fit points or control vertices are replaced by the new fit points. Errors for a tolerance
// that is not positive.
Result<EntityEdit> refitSpline(const Entity& e, double tolerance);

enum class SplineCvOp { Add, Delete, Move, Weight, Close, Open, Purge, ToPolyline };
// Edits the control vertices of a spline. Add: a vertex is inserted at the curve point closest to
// `a` without changing the curve (open splines); Delete: the vertex nearest to `a` goes and the
// curve is redrawn through the rest; Move: the vertex nearest to `a` goes to `b`; Weight: sets the
// weight of the vertex nearest to `a` to `value`; Close / Open make the control polygon wrap or
// not; Purge forgets the fit points (the curve stays); ToPolyline replaces the spline with a
// polyline within `value` mm (default 0.1) of it. Fit points are dropped by every edit that moves
// the curve. A closed spline keeps its wrapped vertices consistent.
Result<EntityEdit> editSplineCv(const Entity& e, SplineCvOp op, Vec2 a = {}, Vec2 b = {},
                                double value = 0);

// ---- FIND / replace ----------------------------------------------------------------------------

struct TextReplace {
    Entity entity{};  // the entity with every match replaced
    int count = 0;    // matches found (replaced or not)
};
// Text and MText, leader notes, dimension text overrides, block attribute values and table cells.
// Returns nothing when the entity holds no match. An empty `find` never matches. With an empty
// `replacement` the matches are deleted.
std::optional<TextReplace> replaceText(const Entity& e, std::string_view find,
                                       std::string_view replacement, bool matchCase);

}  // namespace tbcad::modify
