#include "tbcad/drafting/filters.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "tbcad/core/units.h"
#include "tbcad/drafting/settings.h"

namespace tbcad::drafting {

OsnapFilter::OsnapFilter(const DraftingEnv* env, const SnapGeometryRegistry* providers)
    : env_(env), providers_(providers ? providers : &SnapGeometryRegistry::instance()) {}

std::optional<Vec2> OsnapFilter::filter(const cmd::PointQuery& query, Vec2 raw,
                                        cmd::FilterFeedback& feedback) {
    if (!env_ || !env_->ready())
        return std::nullopt;
    const DraftingSettings s = DraftingSettings::read(*env_->host);
    if (!s.osnap || s.modes == 0)
        return std::nullopt;
    SnapRequest rq;
    rq.cursor = raw;
    rq.base = query.base;
    rq.apertureMm = s.aperturePx * query.pixelSize;
    rq.modes = s.modes;
    std::vector<SnapCandidate> found =
        findSnapCandidates(*env_->doc, *env_->index, rq, *providers_);
    // A different set of candidates starts the Tab cycle over.
    const bool same =
        found.size() == candidates_.size() &&
        std::equal(found.begin(), found.end(), candidates_.begin(),
                   [](const SnapCandidate& a, const SnapCandidate& b) {
                       return a.mode == b.mode && a.point == b.point;
                   });
    if (!same)
        cycle_ = 0;
    candidates_ = std::move(found);
    if (candidates_.empty())
        return std::nullopt;
    const SnapCandidate& c = candidates_[cycle_ % candidates_.size()];
    feedback.marker = std::string(snapModeName(c.mode));
    feedback.markerPos = c.point;
    feedback.tooltip = std::string(snapModeLabel(c.mode));
    return c.point;
}

void OsnapFilter::cycle() {
    if (candidates_.size() > 1)
        cycle_ = (cycle_ + 1) % candidates_.size();
}

namespace {

std::string formatDistance(const DraftingEnv& env, double mm) {
    UnitFormat uf;
    uf.unit = env.doc ? env.doc->header().units : LengthUnit::Millimeter;
    uf.trimTrailingZeros = true;
    return formatLength(mm, 4, uf);
}

double roundTo(double v, double step) {
    return std::round(v / step) * step;
}

}  // namespace

std::optional<Vec2> PolarOrthoFilter::filter(const cmd::PointQuery& query, Vec2 raw,
                                             cmd::FilterFeedback& feedback) {
    if (!env_ || !env_->host || !query.base)
        return std::nullopt;
    const DraftingSettings s = DraftingSettings::read(*env_->host);
    if (!s.ortho && !s.polar)
        return std::nullopt;
    const Vec2 base = *query.base;
    const Vec2 v = raw - base;
    double d = length(v);
    if (d < 1e-9)
        return std::nullopt;

    Vec2 dir{};
    double angleDeg = 0;
    if (s.ortho) {
        if (std::abs(v.x) >= std::abs(v.y))
            dir = {v.x >= 0 ? 1.0 : -1.0, 0.0};
        else
            dir = {0.0, v.y >= 0 ? 1.0 : -1.0};
        d = std::abs(dir.x != 0 ? v.x : v.y);  // project instead of rotate
        angleDeg = std::atan2(dir.y, dir.x) * 180.0 / kPi;
    } else {
        const double ang = std::atan2(v.y, v.x) * 180.0 / kPi;  // (-180, 180]
        double best = 1e9, bestAngle = 0;
        for (double inc : s.polarIncrementsDeg) {
            const double m = roundTo(ang, inc);
            const double diff = std::abs(ang - m);
            if (diff < best) {
                best = diff;
                bestAngle = m;
            }
        }
        if (best > s.polarToleranceDeg)
            return std::nullopt;
        angleDeg = bestAngle;
        const double rad = bestAngle * kPi / 180.0;
        dir = {std::cos(rad), std::sin(rad)};
        // Exact axis directions must not carry cos(90°) noise.
        if (std::abs(dir.x) < 1e-12)
            dir.x = 0;
        if (std::abs(dir.y) < 1e-12)
            dir.y = 0;
    }
    if (s.snap && s.snapSpacing > 0) {
        const double snapped = roundTo(d, s.snapSpacing);
        if (snapped > 0)
            d = snapped;
    }
    const Vec2 p = base + dir * d;
    env_->tracking = DraftingEnv::Tracking{base, raw, dir};

    double shown = angleDeg < 0 ? angleDeg + 360.0 : angleDeg;
    char ang[32];
    std::snprintf(ang, sizeof ang, "%.4g", std::fabs(shown) < 1e-9 ? 0.0 : shown);
    feedback.tooltip = std::string(s.ortho ? "Ortho: " : "Polar: ") + formatDistance(*env_, d) +
                       " < " + ang + "\xC2\xB0";
    const double reach = std::max(d, 1.0) + 4000.0 * std::max(query.pixelSize, 1e-9);
    feedback.guide = std::make_pair(base - dir * (4000.0 * std::max(query.pixelSize, 1e-9)),
                                    base + dir * reach);
    return p;
}

std::optional<Vec2> GridSnapFilter::filter(const cmd::PointQuery&, Vec2 raw,
                                           cmd::FilterFeedback&) {
    if (!env_ || !env_->host)
        return std::nullopt;
    const DraftingSettings s = DraftingSettings::read(*env_->host);
    if (!s.snap || !(s.snapSpacing > 0))
        return std::nullopt;
    return Vec2{roundTo(raw.x, s.snapSpacing), roundTo(raw.y, s.snapSpacing)};
}

}  // namespace tbcad::drafting
