#include "tbcad/drafting/filters.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "tbcad/core/units.h"
#include "tbcad/drafting/settings.h"
#include "tbcad/drafting/ucs.h"
#include "tbcad/index/spatial_index.h"

namespace tbcad::drafting {

Ucs DraftingEnv::effectiveUcs() const {
    if (dynamicUcs)
        return *dynamicUcs;
    return currentUcs(doc, host);
}

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
    const bool same = found.size() == candidates_.size() &&
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
    // Shift flips ORTHO for as long as it is held: temporary ortho when it is off, free
    // (or polar) movement when it is on.
    const bool ortho = s.ortho != query.shift;
    const bool polar = s.polar && !(query.shift && s.ortho);
    if (!ortho && !polar)
        return std::nullopt;
    const Vec2 base = *query.base;
    // Directions are measured in the current UCS: ortho follows its axes and polar angles count
    // from its X axis.
    const Ucs ucs = env_->effectiveUcs();
    const Vec2 v = ucs.dirFromWorld(raw - base);
    double d = length(v);
    if (d < 1e-9)
        return std::nullopt;

    Vec2 dir{};
    double angleDeg = 0;
    if (ortho) {
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
    dir = ucs.dirToWorld(dir);
    const Vec2 p = base + dir * d;
    env_->tracking = DraftingEnv::Tracking{base, raw, dir};

    double shown = angleDeg < 0 ? angleDeg + 360.0 : angleDeg;
    char ang[32];
    std::snprintf(ang, sizeof ang, "%.4g", std::fabs(shown) < 1e-9 ? 0.0 : shown);
    feedback.tooltip = std::string(ortho ? "Ortho: " : "Polar: ") + formatDistance(*env_, d) +
                       " < " + ang + "\xC2\xB0";
    const double reach = std::max(d, 1.0) + 4000.0 * std::max(query.pixelSize, 1e-9);
    feedback.guide =
        std::make_pair(base - dir * (4000.0 * std::max(query.pixelSize, 1e-9)), base + dir * reach);
    return p;
}

std::optional<Vec2> GridSnapFilter::filter(const cmd::PointQuery&, Vec2 raw, cmd::FilterFeedback&) {
    if (!env_ || !env_->host)
        return std::nullopt;
    const DraftingSettings s = DraftingSettings::read(*env_->host);
    if (!s.snap || !(s.snapSpacing > 0))
        return std::nullopt;
    const Ucs ucs = env_->effectiveUcs();
    const Vec2 u = ucs.fromWorld(raw);
    return ucs.toWorld({roundTo(u.x, s.snapSpacing), roundTo(u.y, s.snapSpacing)});
}

}  // namespace tbcad::drafting

namespace tbcad::drafting {

namespace {

// Closest straight edge of `e` to `p`: its start and end, and the distance. False for shapes
// without straight edges.
bool nearestEdge(const Entity& e, Vec2 p, Vec2& a, Vec2& b, double& dist) {
    bool found = false;
    auto consider = [&](Vec2 s, Vec2 t) {
        const Vec2 st = t - s;
        const double len2 = lengthSquared(st);
        if (len2 < 1e-18)
            return;
        const double u = std::clamp(dot(p - s, st) / len2, 0.0, 1.0);
        const double d = length(p - (s + st * u));
        if (!found || d < dist) {
            a = s;
            b = t;
            dist = d;
            found = true;
        }
    };
    if (const auto* l = std::get_if<LineData>(&e.data)) {
        consider(l->a, l->b);
    } else if (const auto* pl = std::get_if<PolylineData>(&e.data)) {
        const std::size_t n = pl->v.size();
        const std::size_t segs = pl->closed ? n : (n ? n - 1 : 0);
        for (std::size_t i = 0; i < segs; ++i)
            if (std::abs(pl->v[i].bulge) < 1e-12)  // arc segments have no single direction
                consider(pl->v[i].p, pl->v[(i + 1) % n].p);
    }
    return found;
}

}  // namespace

std::optional<Vec2> DynamicUcsFilter::filter(const cmd::PointQuery& query, Vec2 raw,
                                             cmd::FilterFeedback& feedback) {
    if (!env_)
        return std::nullopt;
    env_->dynamicUcs.reset();
    if (!env_->ready() || !DraftingSettings::read(*env_->host).dynUcs)
        return std::nullopt;
    const double aperture = DraftingSettings::read(*env_->host).aperturePx * query.pixelSize;
    Vec2 ba{}, bb{};
    double best = aperture;
    bool found = false;
    env_->index->queryPoint(raw, aperture, [&](EntityId id, const BBox2&) {
        const Entity* e = env_->doc->find(id);
        Vec2 a{}, b{};
        double d = 0;
        if (e && nearestEdge(*e, raw, a, b, d) && d <= best) {
            best = d;
            ba = a;
            bb = b;
            found = true;
        }
        return true;
    });
    if (!found)
        return std::nullopt;
    Ucs u;
    u.origin = ba;
    u.angle = std::atan2(bb.y - ba.y, bb.x - ba.x);
    env_->dynamicUcs = u;
    feedback.tooltip = "UCS: edge";
    return std::nullopt;
}

}  // namespace tbcad::drafting
