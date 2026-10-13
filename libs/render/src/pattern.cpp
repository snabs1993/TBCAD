#include "tbcad/render/pattern.h"

#include <cmath>

namespace tbcad::render {

double DashPattern::period() const {
    double p = 0;
    for (double e : elements)
        p += std::abs(e);
    return p;
}

bool DashPattern::solidAt(double pixelsPerMm, double minPeriodPx) const {
    double p = period();
    return p <= 0 || p * pixelsPerMm < minPeriodPx;
}

bool DashPattern::isDash(double d, double onePixelMm) const {
    double p = period();
    if (p <= 0)
        return true;
    double s = std::fmod(d, p);
    if (s < 0)
        s += p;
    double acc = 0;
    for (double e : elements) {
        if (e == 0) {  // dot
            if (s >= acc && s < acc + onePixelMm)
                return true;
            continue;
        }
        double len = std::abs(e);
        if (s < acc + len)
            return e > 0;
        acc += len;
    }
    return false;
}

double wrapDashDistance(double d, double period) {
    if (period <= 0)
        return d;
    double s = std::fmod(d, period);
    return s < 0 ? s + period : s;
}

}  // namespace tbcad::render
