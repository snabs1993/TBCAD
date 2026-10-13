#pragma once

#include <memory>

#include "tbcad/core/entities.h"
#include "tbcad/geom/curve.h"

namespace tbcad::geom::detail {

// Spline evaluators are expensive to build and entities are immutable values, so a small
// per-thread cache keyed by content serves repeated hit tests and snaps. Null on failure.
std::shared_ptr<const SplineCurve> cachedSpline(const SplineData& data);

double arcSweep(const ArcData& a);
double ellipseSweep(const EllipseData& e);
inline CurveRef arcCurve(const ArcData& a) {
    return CurveRef::arc(a.center, a.radius, a.startAngle, arcSweep(a));
}
inline CurveRef ellipseCurve(const EllipseData& e) {
    return CurveRef::ellipse(e.center, e.majorAxis, e.ratio, e.startParam, ellipseSweep(e));
}

}  // namespace tbcad::geom::detail
