#pragma once

#include <optional>
#include <vector>

#include "tbcad/core/core.h"
#include "tbcad/geom/geom.h"

namespace tbcad::modify {

// What an editing algorithm wants done to its source entity. The command layer applies it in one
// transaction: `modified` replaces the source's data (the id is kept), `added` entities are new,
// `eraseSource` removes the source. All results carry the source's properties.
struct EntityEdit {
    std::optional<Entity> modified{};
    std::vector<Entity> added{};
    bool eraseSource = false;
};

// ---- Travel-direction segments ------------------------------------------------------------------

// One polyline segment in travel direction: a line, or a bulge arc (bulge = tan(sweep / 4),
// positive = counter-clockwise).
struct Seg {
    Vec2 p0{}, p1{};
    double bulge = 0;
    bool isArc() const { return std::abs(bulge) > 1e-12; }
};

struct ArcGeom {
    Vec2 center{};
    double radius = 0;
    double startAngle = 0;  // angle of p0 about the center
    double theta = 0;       // signed sweep in travel direction
};
std::optional<ArcGeom> arcGeom(const Seg& s);

Vec2 segPoint(const Seg& s, double u);    // u in [0, 1] along travel; may leave the range
Vec2 segTangent(const Seg& s, double u);  // unit, in travel direction
double segLength(const Seg& s);
// Closest point parameter, clamped to [0, 1].
double segParam(const Seg& s, Vec2 p);
// Sub-segment between u0 < u1.
Seg segSub(const Seg& s, double u0, double u1);
// Same curve with the end point moved to `p` (on the extended curve, further along the travel
// direction) / the start point moved to `p` (before the start).
Seg segWithEnd(const Seg& s, Vec2 p);
Seg segWithStart(const Seg& s, Vec2 p);
Seg segReversed(const Seg& s);
// Curve view for intersections: a line, or the counter-clockwise arc.
geom::CurveRef segCurve(const Seg& s);
// Local parameter in travel direction (unclamped) of a point on the segment's extended curve.
double segParamUnbounded(const Seg& s, Vec2 p);

// Removes consecutive coincident vertices (and a closing duplicate), so that every segment has a
// positive length.
PolylineData cleanPolyline(const PolylineData& pl);
std::vector<Seg> polylineSegs(const PolylineData& cleaned);
// Builds an open or closed polyline from a connected chain of segments. A closed chain's last
// segment ends at the first one's start.
PolylineData polylineFromSegs(const std::vector<Seg>& chain, bool closed,
                              const PolylineData* like = nullptr);
// The part of a segment list from s0 to s1 (global parameter = index + local u, s0 < s1).
std::vector<Seg> sliceSegs(const std::vector<Seg>& segs, double s0, double s1);
// Same, wrapping around the end of a closed list when s1 <= s0.
std::vector<Seg> sliceCyclic(const std::vector<Seg>& segs, double s0, double s1);
// Global parameter (segment index + u) of the point of the segment list closest to p.
double closestGlobalParam(const std::vector<Seg>& segs, Vec2 p);

// ---- Entities
// --------------------------------------------------------------------------------------

// A new entity of the same properties as `like` (the id is cleared).
Entity withData(const Entity& like, EntityData data);
// Line / circle / arc / ellipse entity data for a curve piece; nullopt for splines.
std::optional<EntityData> dataFromCurve(const geom::CurveRef& c);
// Entities whose geometry the editing algorithms understand (Line, Circle, Arc, Polyline,
// Ellipse, Spline). Trim, extend and fillet handle a subset (see each function).
bool isCurveEntity(const Entity& e);

// Sweep within which two points count as the same.
inline constexpr double kEps = 1e-9;
inline bool samePoint(Vec2 a, Vec2 b, double tol = 1e-6) {
    return distance(a, b) <= tol;
}

}  // namespace tbcad::modify
