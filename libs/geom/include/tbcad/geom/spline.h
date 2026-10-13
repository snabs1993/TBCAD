#pragma once

#include <memory>
#include <optional>
#include <span>
#include <vector>

#include "tbcad/core/entities.h"
#include "tbcad/core/math.h"
#include "tbcad/core/result.h"

namespace tbcad::geom {

// Immutable evaluator for a B-spline / NURBS curve of degree 1..15 (de Boor, homogeneous
// coordinates for rational curves). The parameter domain is [tMin(), tMax()].
class SplineCurve {
public:
    // Builds from control points (uses `knots` when present, otherwise a clamped uniform knot
    // vector). When the data holds only fit points they are interpolated first.
    static Result<std::shared_ptr<const SplineCurve>> make(const SplineData& data);

    int degree() const { return degree_; }
    double tMin() const { return tMin_; }
    double tMax() const { return tMax_; }
    const std::vector<double>& knots() const { return knots_; }
    // Distinct knot values inside [tMin, tMax], ascending, including both ends.
    const std::vector<double>& breaks() const { return breaks_; }
    bool rational() const { return rational_; }
    const BBox2& bounds() const { return bounds_; }

    Vec2 pointAt(double t) const;
    // d/dt of the curve (not normalised).
    Vec2 derivativeAt(double t) const;

    // Fixed sampling used for closest-point seeds and numeric intersection. `dev` bounds the
    // distance between the chords and the curve (estimated from chord midpoints).
    const std::vector<double>& sampleParams() const { return sampleT_; }
    const std::vector<Vec2>& samplePoints() const { return sampleP_; }
    double sampleDeviation() const { return sampleDev_; }

private:
    SplineCurve() = default;
    int spanOf(double t) const;
    void basis(int span, double t, int p, const std::vector<double>& u, double* out) const;
    void buildSamples();

    int degree_ = 3;
    std::vector<double> knots_{};
    std::vector<double> breaks_{};
    std::vector<Vec2> ctrl_{};
    std::vector<double> weights_{};                 // empty = non-rational
    std::vector<double> dq_x_{}, dq_y_{}, dq_w_{};  // derivative control points (homogeneous)
    std::vector<double> dknots_{};
    bool rational_ = false;
    double tMin_ = 0, tMax_ = 1;
    BBox2 bounds_{};
    std::vector<double> sampleT_{};
    std::vector<Vec2> sampleP_{};
    double sampleDev_ = 0;
};

struct InterpolateOptions {
    int degree = 3;
    bool closed = false;  // the curve returns to the first fit point (C1 closure)
    // Tangent end conditions (directions; any length). When neither is set the ends are
    // "natural" (zero second derivative). Setting only one uses a natural end for the other.
    std::optional<Vec2> startTangent{};
    std::optional<Vec2> endTangent{};
    bool natural = true;  // false and no tangents: estimate tangents from neighbouring points
};

// Global interpolation through the fit points with chord-length parameterisation. Returns a
// clamped spline whose `fit` field holds the (de-duplicated) input points.
Result<SplineData> interpolateSpline(std::span<const Vec2> fitPoints,
                                     const InterpolateOptions& options = {});

// The part of the spline between parameters ta < tb as a clamped spline of the same degree
// (knot insertion). Fit points are dropped.
Result<SplineData> splineSubcurve(const SplineData& data, double ta, double tb);

}  // namespace tbcad::geom
