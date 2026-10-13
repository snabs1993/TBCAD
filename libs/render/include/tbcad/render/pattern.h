#pragma once

#include <vector>

namespace tbcad::render {

// A linetype dash pattern in millimetres: + dash, - gap, 0 dot (same convention as
// tbcad::Linetype::pattern). The renderer draws a dot as a one pixel long dash at the start of
// the element; it takes no length along the line.
struct DashPattern {
    std::vector<double> elements{};

    bool continuous() const { return period() <= 0; }
    // Sum of |element|; 0 for a continuous pattern.
    double period() const;
    // True when dashes would shimmer at this zoom (the whole period is under `minPeriodPx`
    // pixels), so the line should be drawn solid.
    bool solidAt(double pixelsPerMm, double minPeriodPx = 2.0) const;
    // Reference implementation of the fragment shader's test: is the point at distance `d`
    // (mm, along the line) inside a dash? `onePixelMm` is the size of a pixel in mm.
    bool isDash(double d, double onePixelMm) const;
};

// Distance along a polyline reduced modulo the pattern period, so it stays small and exact in
// a float vertex attribute. Returns `d` unchanged for a continuous pattern.
double wrapDashDistance(double d, double period);

}  // namespace tbcad::render
