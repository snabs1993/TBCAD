#pragma once

#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include "tbcad/core/entities.h"
#include "tbcad/core/math.h"
#include "tbcad/core/result.h"
#include "tbcad/geom/spline.h"

namespace tbcad::geom {

// A lightweight value view of one smooth curve piece: a line segment, a circular arc (or full
// circle), an elliptical arc (or full ellipse) or a spline span. Cheap to copy (a spline shares
// its immutable evaluator).
//
// Parameter conventions (see docs/geom.md):
//   Line     t in [0, 1] from a to b
//   Arc      t = angle (rad), counter-clockwise, in [startAngle, startAngle + sweep]
//   Ellipse  t = parametric angle, center + major*cos(t) + perp(major)*ratio*sin(t)
//   Spline   t = knot parameter
class CurveRef {
public:
    enum class Kind { Line, Arc, Ellipse, Spline };

    CurveRef() = default;
    static CurveRef line(Vec2 a, Vec2 b);
    // sweep in (0, 2*pi]; 2*pi is a full circle.
    static CurveRef arc(Vec2 center, double radius, double startAngle, double sweep);
    static CurveRef circle(Vec2 center, double radius) { return arc(center, radius, 0, kTwoPi); }
    // `major` is the vector from the center to the end of the major axis; sweep in (0, 2*pi].
    static CurveRef ellipse(Vec2 center, Vec2 major, double ratio, double startParam, double sweep);
    static Result<CurveRef> spline(const SplineData& data);
    static CurveRef spline(std::shared_ptr<const SplineCurve> curve, double t0, double t1);

    Kind kind() const { return kind_; }
    bool isLine() const { return kind_ == Kind::Line; }

    // Geometry accessors (meaning depends on kind).
    Vec2 lineA() const { return p_; }
    Vec2 lineB() const { return q_; }
    Vec2 center() const { return p_; }
    double radius() const { return r_; }  // Arc
    double startAngle() const { return t0_; }
    double sweep() const { return sweep_; }
    Vec2 majorAxis() const { return q_; }  // Ellipse
    double ratio() const { return r_; }    // Ellipse
    const std::shared_ptr<const SplineCurve>& splineCurve() const { return sp_; }

    double t0() const { return t0_; }
    double t1() const { return t0_ + sweep_; }
    bool isClosed() const;
    Vec2 startPoint() const { return pointAt(t0()); }
    Vec2 endPoint() const { return pointAt(t1()); }

    Vec2 pointAt(double t) const;
    Vec2 derivativeAt(double t) const;
    // Unit tangent in the direction of increasing t (zero vector when degenerate).
    Vec2 tangentAt(double t) const;
    // Parameter of the closest point of the (bounded) curve to p.
    double paramAt(Vec2 p) const;
    Vec2 closestPoint(Vec2 p) const { return pointAt(paramAt(p)); }
    double distanceTo(Vec2 p) const { return distance(closestPoint(p), p); }
    // For periodic kinds (arc, ellipse): the parameter congruent to t that lies in
    // [t0, t0 + 2*pi). Identity for the others.
    double wrapParam(double t) const;
    // True when t is within the parameter range, allowing a slack of `slack` mm of arc length.
    bool containsParam(double t, double slack = tol::kLinear) const;

    double length() const;
    double lengthBetween(double ta, double tb) const;
    // Parameter at arc length s from the start (clamped to [0, length()]).
    double paramAtLength(double s) const;
    // The n + 1 points at arc lengths length() * i / n, i = 0..n, found in one sweep along the
    // curve (the same points as pointAt(paramAtLength(...)) for each i, without a pass per point).
    std::vector<Vec2> pointsAtEqualLength(int n) const;
    BBox2 bounds() const;

    // The part between ta < tb. Lines are re-normalised to [0, 1]; the other kinds keep their
    // parameter values.
    Result<CurveRef> subcurve(double ta, double tb) const;
    // Two pieces, split at an interior parameter (error when t is within tolerance of an end or
    // the curve is closed with t at its seam).
    Result<std::pair<CurveRef, CurveRef>> split(double t) const;

private:
    Kind kind_ = Kind::Line;
    Vec2 p_{};
    Vec2 q_{1, 0};
    double r_ = 0;
    double t0_ = 0;
    double sweep_ = 1;
    std::shared_ptr<const SplineCurve> sp_{};
};

// Appends a polyline approximation (including both end points) whose maximum deviation from the
// curve is about `tol` mm. Optionally records the parameter of each point.
void flatten(const CurveRef& curve, double tol, std::vector<Vec2>& out,
             std::vector<double>* params = nullptr);

// ---- Polylines -------------------------------------------------------------------------------

struct BulgeArc {
    Vec2 center{};
    double radius = 0;
    double startAngle = 0;  // counter-clockwise
    double sweep = 0;       // (0, 2*pi)
    bool reversed = false;  // the polyline travels the arc clockwise (negative bulge)
};
// Arc described by a bulge between p0 and p1. Empty for a (near) zero bulge or coincident points.
std::optional<BulgeArc> bulgeArc(Vec2 p0, Vec2 p1, double bulge);
// Bulge (tan(sweep/4), signed) of the arc that runs from a through b to c. Empty when the three
// points are collinear.
std::optional<double> bulgeThrough(Vec2 a, Vec2 b, Vec2 c);
// Counter-clockwise arc through three points (a, b, c in travel order). Error when collinear.
Result<ArcData> arcFromThreePoints(Vec2 a, Vec2 b, Vec2 c);

struct Segment {
    CurveRef curve{};
    int index = 0;          // index of the starting vertex
    bool reversed = false;  // travel direction is opposite to the curve's parameter direction
    double bulge = 0;
    Vec2 p0{}, p1{};  // end points in travel order

    // u in [0, 1] along the direction of travel.
    Vec2 pointAtLocal(double u) const { return curve.pointAt(toCurveParam(u)); }
    double toCurveParam(double u) const;
    // Clamped to [0, 1].
    double toLocal(double t) const;
    // Like toLocal but not clamped: parameters before the start give u < 0, after the end u > 1
    // (for an extended arc, whichever end is nearer in angle decides).
    double toLocalUnbounded(double t) const;
};

// All non-degenerate segments (a closing segment is included for closed polylines); zero-length
// segments are skipped but keep their vertex index.
std::vector<Segment> segments(const PolylineData& pl);

// Number of segments including the closing one.
inline int segmentCount(const PolylineData& pl) {
    int n = static_cast<int>(pl.v.size());
    if (n < 2)
        return 0;
    return pl.closed ? n : n - 1;
}

// ---- Intersections ---------------------------------------------------------------------------

struct Intersection {
    Vec2 p{};
    double tA = 0, tB = 0;  // parameters on the first / second curve
};

// Appends the intersections of two curves. With extendA / extendB a line is treated as an
// unbounded line, an arc as a full circle and an ellipse as a full ellipse (splines are never
// extended). Collinear / concentric overlaps report the end points of the overlap; tangent
// contacts report one point. Parameters of points outside the bounded range are reported
// normalised (angles wrap into [t0, t0 + 2*pi), line parameters may leave [0, 1]).
void intersect(const CurveRef& a, const CurveRef& b, std::vector<Intersection>& out,
               bool extendA = false, bool extendB = false);

}  // namespace tbcad::geom
