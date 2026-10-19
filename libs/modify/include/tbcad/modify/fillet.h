#pragma once

#include <cstddef>
#include <optional>
#include <vector>

#include "tbcad/modify/common.h"

namespace tbcad::modify {

// Result of an operation on two entities: new values for the inputs (when they changed) and new
// entities. All results carry the properties of the entity they come from; the fillet arc and the
// chamfer line take the first entity's.
struct PairEdit {
    std::optional<Entity> modifiedA{};
    std::optional<Entity> modifiedB{};
    std::vector<Entity> added{};
};

struct FilletOptions {
    double radius = 0;  // 0 = sharp corner
    bool trim = true;   // false: only add the arc, leave the inputs alone
};

// Fillet between two lines / arcs / circles. The pick points choose which of the possible fillets
// is made (the one nearest to them) and which part of each input is kept: the one that contains
// the pick. Inputs are trimmed or extended to the tangent points. Radius 0 extends / trims both to
// their intersection. Parallel lines get a semicircle (the radius is ignored). Circles are never
// trimmed. Errors: collinear or concentric inputs, no solution, a trimmed result without length.
Result<PairEdit> filletEntities(const Entity& a, Vec2 pickA, const Entity& b, Vec2 pickB,
                                const FilletOptions& options);

// Rounds the corner at `vertex` of a polyline (index into the cleaned polyline, see
// cleanPolyline); open polylines have corners at 1 .. n-2, closed ones at every vertex. Returns the
// new polyline. A corner whose segments are too short for the radius is an error.
Result<Entity> filletPolylineVertex(const Entity& polyline, std::size_t vertex, double radius);
// Rounds every corner that is long enough for the radius (others stay sharp). Error when nothing
// could be rounded and the radius is > 0.
Result<Entity> filletPolyline(const Entity& polyline, double radius);

struct ChamferOptions {
    double distance1 = 0;
    double distance2 = 0;  // distance-distance method
    bool byAngle = false;  // distance-angle: distance1 along the first line + angle
    double angle = 0;      // radians, between the chamfer and the first line
    bool trim = true;
};

// Chamfer between two straight lines (distance 1 on the first). Errors: parallel lines, a distance
// longer than the line it cuts.
Result<PairEdit> chamferEntities(const Entity& a, Vec2 pickA, const Entity& b, Vec2 pickB,
                                 const ChamferOptions& options);
// Chamfers a polyline corner / all corners (straight segments only).
Result<Entity> chamferPolylineVertex(const Entity& polyline, std::size_t vertex,
                                     const ChamferOptions& options);
Result<Entity> chamferPolyline(const Entity& polyline, const ChamferOptions& options);

// Index of the polyline's cleaned segment nearest to the point.
std::size_t nearestSegmentIndex(const PolylineData& polyline, Vec2 p);
// The vertex shared by two segments of the cleaned polyline, when they are neighbours.
std::optional<std::size_t> sharedVertex(const PolylineData& polyline, std::size_t segA,
                                        std::size_t segB);

// Two straight segments of a polyline with exactly one arc segment between them (as numbered by
// nearestSegmentIndex): the arc is removed and the two lines are extended or cut back to meet at a
// corner, the way FILLET and CHAMFER treat them. Returns the new polyline and the index of the new
// corner, which filletPolylineVertex / chamferPolylineVertex then round or cut. Errors: the
// segments are not two lines around one arc, or the lines do not meet ahead of the first one and
// behind the second.
struct ArcRemoval {
    Entity polyline;
    std::size_t vertex = 0;
};
Result<ArcRemoval> removeArcBetween(const Entity& polyline, std::size_t segA, std::size_t segB);

}  // namespace tbcad::modify
