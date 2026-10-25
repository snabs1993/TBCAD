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

namespace {
// The system the drafting aids work in: the UCS turned by SNAPANG.
Ucs aidsUcs(const DraftingEnv& env, const DraftingSettings& s) {
    Ucs u = env.effectiveUcs();
    u.angle += s.snapAngleDeg * kPi / 180.0;
    return u;
}
}  // namespace

namespace {
bool nearestEdge(const Entity& e, Vec2 p, Vec2& a, Vec2& b, double& dist);
std::string formatDistance(const DraftingEnv& env, double mm);
}  // namespace

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
    if (s.modes & maskOf(SnapMode::Extension)) {
        // Resting on an end acquires it even while an endpoint snap shows there; a real
        // candidate beats the extension.
        cmd::FilterFeedback ext;
        auto p = extension(query, raw, rq.apertureMm, ext);
        if (p && candidates_.empty()) {
            feedback = std::move(ext);
            return p;
        }
    }
    if (candidates_.empty() && (s.modes & maskOf(SnapMode::Parallel)) && query.base)
        return parallel(query, raw, rq.apertureMm, feedback);
    if (query.click) {
        parallelEdge_.reset();
        extEnds_.clear();
    }
    if (candidates_.empty())
        return std::nullopt;
    const SnapCandidate& c = candidates_[cycle_ % candidates_.size()];
    feedback.marker = std::string(snapModeName(c.mode));
    feedback.markerPos = c.point;
    feedback.tooltip = std::string(snapModeLabel(c.mode));
    return c.point;
}

std::optional<Vec2> OsnapFilter::parallel(const cmd::PointQuery& query, Vec2 raw, double aperture,
                                          cmd::FilterFeedback& feedback) {
    const Vec2 base = *query.base;
    // Resting on a straight edge acquires it.
    double best = aperture;
    env_->index->queryPoint(raw, aperture, [&](EntityId id, const BBox2&) {
        const Entity* e = env_->doc->find(id);
        Vec2 a{}, b{};
        double d = 0;
        if (e && nearestEdge(*e, raw, a, b, d) && d <= best) {
            best = d;
            parallelEdge_ = std::make_pair(a, b);
        }
        return true;
    });
    const auto edge = parallelEdge_;
    if (query.click)
        parallelEdge_.reset();
    if (!edge)
        return std::nullopt;
    const Vec2 dir = normalized(edge->second - edge->first);
    const Vec2 v = raw - base;
    const double t = dot(v, dir);
    const Vec2 p = base + dir * t;
    if (length(raw - p) > aperture || std::abs(t) < aperture)
        return std::nullopt;
    const double reach = 4000.0 * std::max(query.pixelSize, 1e-9);
    const Vec2 s = t >= 0 ? dir : dir * -1.0;
    feedback.marker = std::string(snapModeName(SnapMode::Parallel));
    feedback.markerPos = (edge->first + edge->second) * 0.5;
    feedback.tooltip = std::string(snapModeLabel(SnapMode::Parallel));
    feedback.guide = std::make_pair(base, base + s * (std::abs(t) + reach));
    return p;
}

std::optional<Vec2> OsnapFilter::extension(const cmd::PointQuery& query, Vec2 raw, double aperture,
                                           cmd::FilterFeedback& feedback) {
    // Acquire the end the cursor rests on.
    auto acquire = [&](const ExtEnd& x) {
        for (const ExtEnd& e : extEnds_)
            if (distance(e.end, x.end) < 1e-9 && e.arc == x.arc)
                return;
        extEnds_.push_back(x);
        if (extEnds_.size() > 2)
            extEnds_.erase(extEnds_.begin());
    };
    env_->index->queryPoint(raw, aperture, [&](EntityId id, const BBox2&) {
        const Entity* e = env_->doc->find(id);
        if (!e)
            return true;
        auto lineEnd = [&](Vec2 from, Vec2 end) {
            if (distance(raw, end) <= aperture && distance(from, end) > 1e-9)
                acquire(ExtEnd{end, normalized(end - from)});
        };
        if (const auto* l = entityAs<LineData>(*e)) {
            lineEnd(l->a, l->b);
            lineEnd(l->b, l->a);
        } else if (const auto* pl = entityAs<PolylineData>(*e)) {
            const auto& v = pl->v;
            if (!pl->closed && v.size() >= 2) {
                if (std::abs(v[0].bulge) < 1e-12)
                    lineEnd(v[1].p, v[0].p);
                if (std::abs(v[v.size() - 2].bulge) < 1e-12)
                    lineEnd(v[v.size() - 2].p, v.back().p);
            }
        } else if (const auto* a = entityAs<ArcData>(*e)) {
            for (double ang : {a->startAngle, a->endAngle}) {
                const Vec2 end = a->center + Vec2{std::cos(ang), std::sin(ang)} * a->radius;
                if (distance(raw, end) <= aperture) {
                    ExtEnd x{end, {}, true, a->center, a->radius, a->startAngle, a->endAngle};
                    acquire(x);
                }
            }
        }
        return true;
    });
    const std::vector<ExtEnd> ends = extEnds_;
    if (query.click)
        extEnds_.clear();
    if (ends.empty())
        return std::nullopt;

    // Each end's path: the line beyond the end, or the circle outside the arc.
    struct Hit {
        Vec2 p;
        double off;
        const ExtEnd* e;
    };
    std::vector<Hit> hits;
    for (const ExtEnd& e : ends) {
        if (!e.arc) {
            const double t = dot(raw - e.end, e.dir);
            if (t <= aperture)  // on the object itself, or behind it
                continue;
            const Vec2 p = e.end + e.dir * t;
            hits.push_back({p, distance(raw, p), &e});
        } else {
            const Vec2 r = raw - e.center;
            const double len = length(r);
            if (len < 1e-12)
                continue;
            const Vec2 p = e.center + r * (e.radius / len);
            // Inside the arc's sweep it is the arc itself, not its extension.
            double ang = std::atan2(r.y, r.x) - e.startAngle;
            double sweep = e.endAngle - e.startAngle;
            const double tau = 2 * kPi;
            ang = std::fmod(std::fmod(ang, tau) + tau, tau);
            sweep = std::fmod(std::fmod(sweep, tau) + tau, tau);
            if (ang <= sweep || distance(p, e.end) <= aperture)
                continue;
            hits.push_back({p, distance(raw, p), &e});
        }
    }
    // Two straight extensions that cross near the cursor: their crossing.
    if (ends.size() == 2 && !ends[0].arc && !ends[1].arc) {
        const double den = cross(ends[0].dir, ends[1].dir);
        if (std::abs(den) > 1e-9) {
            const double t = cross(ends[1].end - ends[0].end, ends[1].dir) / den;
            const double u = cross(ends[1].end - ends[0].end, ends[0].dir) / den;
            const Vec2 x = ends[0].end + ends[0].dir * t;
            if (t > 0 && u > 0 && distance(raw, x) <= aperture) {
                feedback.marker = std::string(snapModeName(SnapMode::Extension));
                feedback.markerPos = x;
                feedback.tooltip = "Extension intersection";
                feedback.guide = std::make_pair(ends[0].end, x);
                return x;
            }
        }
    }
    const Hit* best = nullptr;
    for (const Hit& h : hits)
        if (h.off <= aperture && (!best || h.off < best->off))
            best = &h;
    if (!best)
        return std::nullopt;
    feedback.marker = std::string(snapModeName(SnapMode::Extension));
    feedback.markerPos = best->p;
    char buf[64];
    if (best->e->arc) {
        std::snprintf(buf, sizeof buf, "Extension: R %.4g", best->e->radius);
        feedback.tooltip = buf;
    } else {
        feedback.tooltip = "Extension: " + formatDistance(*env_, distance(best->e->end, best->p));
        feedback.guide = std::make_pair(best->e->end, best->p);
    }
    return best->p;
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
    return formatLength(mm, 2, uf);
}

double roundTo(double v, double step) {
    return std::round(v / step) * step;
}

}  // namespace

namespace {

// Where ORTHO / polar tracking send the cursor from the base point: the direction (in the UCS),
// its angle, the distance along it, and which of them was at work.
struct Constrained {
    Vec2 dir{};
    double angleDeg = 0;
    double d = 0;
    bool ortho = false;
    bool iso = false;
    bool relative = false;             // polar angle measured from the last segment
    std::optional<double> shownDeg{};  // the polar angle to show, when not angleDeg
    DraftingSettings s{};
    Ucs ucs{};
};

std::optional<Constrained> constrain(const DraftingEnv* env, const cmd::PointQuery& query,
                                     Vec2 raw) {
    if (!env || !env->host || !query.base)
        return std::nullopt;
    Constrained c;
    c.s = DraftingSettings::read(*env->host);
    const DraftingSettings& s = c.s;
    // Shift flips ORTHO for as long as it is held: temporary ortho when it is off, free
    // (or polar) movement when it is on.
    const bool ortho = s.ortho != query.shift;
    const bool polar = s.polar && !(query.shift && s.ortho);
    if (!ortho && !polar)
        return std::nullopt;
    const Vec2 base = *query.base;
    // Directions are measured in the current UCS: ortho follows its axes and polar angles count
    // from its X axis.
    c.ucs = aidsUcs(*env, s);
    const Vec2 v = c.ucs.dirFromWorld(raw - base);
    double d = length(v);
    if (d < 1e-9)
        return std::nullopt;

    Vec2 dir{};
    double angleDeg = 0;
    if (ortho && s.isoPlane != IsoPlane::Off) {
        // Isometric: the two axes of the plane, both ways.
        const auto [a1, a2] = isoAxesDeg(s.isoPlane);
        double best = -1;
        for (double a : {a1, a2}) {
            const Vec2 axis{std::cos(a * kPi / 180.0), std::sin(a * kPi / 180.0)};
            const double proj = dot(v, axis);
            if (std::abs(proj) > best) {
                best = std::abs(proj);
                dir = proj >= 0 ? axis : axis * -1.0;
                angleDeg = proj >= 0 ? a : a - 180.0;
            }
        }
        d = best;
    } else if (ortho) {
        if (std::abs(v.x) >= std::abs(v.y))
            dir = {v.x >= 0 ? 1.0 : -1.0, 0.0};
        else
            dir = {0.0, v.y >= 0 ? 1.0 : -1.0};
        d = std::abs(dir.x != 0 ? v.x : v.y);  // project instead of rotate
        angleDeg = std::atan2(dir.y, dir.x) * 180.0 / kPi;
    } else {
        // Relative polar: angles count from the direction of the last segment.
        double zero = 0;
        if (s.polarRelative && query.lastSegment) {
            const Vec2 seg = c.ucs.dirFromWorld(*query.lastSegment);
            zero = std::atan2(seg.y, seg.x) * 180.0 / kPi;
            c.relative = true;
        }
        // Angle from `zero`, wrapped into (-180, 180].
        auto wrap = [](double a) {
            a = std::fmod(a, 360.0);
            if (a > 180)
                a -= 360;
            if (a <= -180)
                a += 360;
            return a;
        };
        const double ang = wrap(std::atan2(v.y, v.x) * 180.0 / kPi - zero);
        double best = 1e9, bestAngle = 0;
        for (double inc : s.polarIncrementsDeg) {
            const double m = roundTo(ang, inc);
            const double diff = std::abs(ang - m);
            if (diff < best) {
                best = diff;
                bestAngle = m;
            }
        }
        // Additional angles track only themselves, not their multiples.
        for (double a : s.polarAdditionalDeg) {
            const double diff = std::abs(wrap(ang - a));
            if (diff < best) {
                best = diff;
                bestAngle = wrap(a);
            }
        }
        if (best > s.polarToleranceDeg)
            return std::nullopt;
        c.shownDeg = bestAngle;
        angleDeg = bestAngle + zero;
        const double rad = angleDeg * kPi / 180.0;
        dir = {std::cos(rad), std::sin(rad)};
        // Exact axis directions must not carry cos(90°) noise.
        if (std::abs(dir.x) < 1e-12)
            dir.x = 0;
        if (std::abs(dir.y) < 1e-12)
            dir.y = 0;
    }
    c.dir = dir;
    c.angleDeg = angleDeg;
    c.d = d;
    c.ortho = ortho;
    c.iso = ortho && s.isoPlane != IsoPlane::Off;
    return c;
}

}  // namespace

std::optional<Vec2> orthoPolarDirection(const DraftingEnv* env, const cmd::PointQuery& query,
                                        Vec2 raw) {
    auto c = constrain(env, query, raw);
    if (!c)
        return std::nullopt;
    return c->ucs.dirToWorld(c->dir);
}

std::optional<Vec2> PolarOrthoFilter::filter(const cmd::PointQuery& query, Vec2 raw,
                                             cmd::FilterFeedback& feedback) {
    auto c = constrain(env_, query, raw);
    if (!c)
        return std::nullopt;
    const DraftingSettings& s = c->s;
    const Vec2 base = *query.base;
    double d = c->d;
    const double angleDeg = c->shownDeg ? *c->shownDeg : c->angleDeg;
    // Snap steps along the tracking line: the polar distance for polar snap, else the snap X
    // spacing.
    const double step = s.polarSnap && s.polarDistance > 0 ? s.polarDistance : s.snapSpacing;
    if (s.snap && step > 0) {
        const double snapped = roundTo(d, step);
        if (snapped > 0)
            d = snapped;
    }
    const Vec2 dir = c->ucs.dirToWorld(c->dir);
    const Vec2 p = base + dir * d;
    env_->tracking = DraftingEnv::Tracking{base, raw, dir};

    double shown = angleDeg < 0 ? angleDeg + 360.0 : angleDeg;
    char ang[32];
    std::snprintf(ang, sizeof ang, "%.4g",
                  std::fabs(shown) < 1e-9 ? 0.0 : std::round(shown * 100) / 100);
    feedback.tooltip = std::string(c->ortho      ? (c->iso ? "Iso ortho: " : "Ortho: ")
                                   : c->relative ? "Relative polar: "
                                                 : "Polar: ") +
                       formatDistance(*env_, d) + " < " + ang + "\xC2\xB0";
    const double reach = std::max(d, 1.0) + 4000.0 * std::max(query.pixelSize, 1e-9);
    feedback.guide =
        std::make_pair(base - dir * (4000.0 * std::max(query.pixelSize, 1e-9)), base + dir * reach);
    return p;
}

std::optional<Vec2> GridSnapFilter::filter(const cmd::PointQuery&, Vec2 raw, cmd::FilterFeedback&) {
    if (!env_ || !env_->host)
        return std::nullopt;
    const DraftingSettings s = DraftingSettings::read(*env_->host);
    // Polar snap only acts along polar / ortho tracking (PolarOrthoFilter); the free cursor
    // does not jump to a lattice.
    if (!s.snap || s.polarSnap || !(s.snapSpacing > 0) || !(s.snapSpacingY > 0))
        return std::nullopt;
    const Ucs ucs = aidsUcs(*env_, s);
    const Vec2 u = ucs.fromWorld(raw) - s.snapBase;
    if (s.isoPlane != IsoPlane::Off) {
        // The lattice of the plane: steps of the spacing along both of its axes.
        const auto [a1, a2] = isoAxesDeg(s.isoPlane);
        const Vec2 b1{std::cos(a1 * kPi / 180.0), std::sin(a1 * kPi / 180.0)};
        const Vec2 b2{std::cos(a2 * kPi / 180.0), std::sin(a2 * kPi / 180.0)};
        const double det = b1.x * b2.y - b1.y * b2.x;
        const double i = (u.x * b2.y - u.y * b2.x) / det / s.snapSpacingY;
        const double j = (b1.x * u.y - b1.y * u.x) / det / s.snapSpacingY;
        const double ri = std::round(i), rj = std::round(j);
        return ucs.toWorld((b1 * ri + b2 * rj) * s.snapSpacingY + s.snapBase);
    }
    return ucs.toWorld(Vec2{roundTo(u.x, s.snapSpacing), roundTo(u.y, s.snapSpacingY)} +
                       s.snapBase);
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

namespace tbcad::drafting {

void OtrackFilter::acquire(Vec2 p, double tolerance) {
    for (const Vec2& a : acquired_)
        if (length(a - p) <= tolerance)
            return;
    if (acquired_.size() >= 3)
        acquired_.erase(acquired_.begin());
    acquired_.push_back(p);
}

std::optional<Vec2> OtrackAcquirer::filter(const cmd::PointQuery& query, Vec2,
                                           cmd::FilterFeedback&) {
    if (!env_ || !env_->host || !tracker_ || !osnap_)
        return std::nullopt;
    if (query.click) {
        tracker_->clear();
        return std::nullopt;
    }
    const DraftingSettings s = DraftingSettings::read(*env_->host);
    if (!s.otrack || !s.osnap) {
        tracker_->clear();
        return std::nullopt;
    }
    // The candidates are those of the previous pointer move: one move behind is invisible.
    const auto& c = osnap_->candidates();
    if (!c.empty()) {
        const double tol = 2.0 * std::max(query.pixelSize, 1e-9);
        if (!query.base || length(c.front().point - *query.base) > tol)
            tracker_->acquire(c.front().point, tol);
    }
    return std::nullopt;
}

std::optional<Vec2> OtrackFilter::filter(const cmd::PointQuery& query, Vec2 raw,
                                         cmd::FilterFeedback& feedback) {
    rays_.clear();
    if (!env_ || !env_->host)
        return std::nullopt;
    const DraftingSettings s = DraftingSettings::read(*env_->host);
    // Temporary tracking points (TT) work for the next point whether OTRACK is on or not.
    const std::vector<Vec2> temp = env_->tempTrack;
    if (query.click)
        env_->tempTrack.clear();
    if (!s.otrack && temp.empty())
        return std::nullopt;
    if (acquired_.empty() && temp.empty() && !query.base)
        return std::nullopt;
    const Ucs ucs = aidsUcs(*env_, s);
    const double px = std::max(query.pixelSize, 1e-9);
    const double tol = 8.0 * px;

    // Directions in UCS terms: the polar increments when POLAR is on, else the four axes.
    std::vector<Vec2> dirs;
    std::vector<double> incs = s.polar ? s.polarIncrementsDeg : std::vector<double>{90.0};
    for (double inc : incs) {
        if (inc <= 0 || inc > 360)
            continue;
        for (double a = 0; a < 360.0 - 1e-9; a += inc) {
            const double r = a * kPi / 180.0;
            Vec2 d{std::cos(r), std::sin(r)};
            if (std::abs(d.x) < 1e-12)
                d.x = 0;
            if (std::abs(d.y) < 1e-12)
                d.y = 0;
            d = ucs.dirToWorld(d);
            bool dup = false;
            for (const Vec2& e : dirs)
                if (std::abs(e.x - d.x) < 1e-9 && std::abs(e.y - d.y) < 1e-9)
                    dup = true;
            if (!dup)
                dirs.push_back(d);
        }
    }

    struct Ray {
        Vec2 origin;
        Vec2 dir;
        bool base;
    };
    std::vector<Ray> rays;
    if (s.otrack)
        for (const Vec2& o : acquired_)
            for (const Vec2& d : dirs)
                rays.push_back({o, d, false});
    for (const Vec2& o : temp)
        for (const Vec2& d : dirs)
            rays.push_back({o, d, false});
    if (query.base && (s.polar || s.ortho))
        for (const Vec2& d : dirs)
            rays.push_back({*query.base, d, true});

    // Rays the cursor is close to.
    std::vector<std::pair<double, std::size_t>> near;
    for (std::size_t i = 0; i < rays.size(); ++i) {
        const Vec2 v = raw - rays[i].origin;
        const double perp = std::abs(rays[i].dir.x * v.y - rays[i].dir.y * v.x);
        if (perp <= tol)
            near.emplace_back(perp, i);
    }
    if (near.empty())
        return std::nullopt;

    std::optional<Vec2> best;
    double bestDist = 1e300;
    std::pair<std::size_t, std::size_t> bestPair{0, 0};
    for (std::size_t a = 0; a < near.size(); ++a)
        for (std::size_t b = a + 1; b < near.size(); ++b) {
            const Ray& r1 = rays[near[a].second];
            const Ray& r2 = rays[near[b].second];
            if (length(r1.origin - r2.origin) < 1e-9)
                continue;
            const double den = r1.dir.x * r2.dir.y - r1.dir.y * r2.dir.x;
            if (std::abs(den) < 1e-6)
                continue;
            const Vec2 w = r2.origin - r1.origin;
            const double t = (w.x * r2.dir.y - w.y * r2.dir.x) / den;
            const Vec2 x = r1.origin + r1.dir * t;
            const double d = length(x - raw);
            if (d <= tol * 1.5 && d < bestDist) {
                bestDist = d;
                best = x;
                bestPair = {near[a].second, near[b].second};
            }
        }
    if (best) {
        for (std::size_t i : {bestPair.first, bestPair.second})
            rays_.emplace_back(rays[i].origin, *best);
        feedback.marker = "intersection";
        feedback.markerPos = *best;
        feedback.tooltip = "Tracking: intersection";
        feedback.guide = rays_.front();
        return best;
    }
    // A single ray: only acquired points (the base point's ray belongs to polar / ortho).
    std::sort(near.begin(), near.end());
    for (const auto& n : near) {
        const Ray& r = rays[n.second];
        if (r.base)
            continue;
        const Vec2 v = raw - r.origin;
        const double t = r.dir.x * v.x + r.dir.y * v.y;
        const Vec2 p = r.origin + r.dir * t;
        rays_.emplace_back(r.origin, p);
        const Vec2 local = ucs.dirFromWorld(r.dir);
        double ang = std::atan2(local.y, local.x) * 180.0 / kPi;
        if (t < 0)
            ang += 180.0;
        if (ang < 0)
            ang += 360.0;
        if (ang >= 360.0 - 1e-9)
            ang -= 360.0;
        char buf[32];
        std::snprintf(buf, sizeof buf, "%.4g", std::round(ang * 100) / 100);
        feedback.tooltip =
            "Tracking: " + formatDistance(*env_, std::abs(t)) + " < " + buf + "\xC2\xB0";
        const double reach = std::abs(t) + 4000.0 * px;
        const Vec2 d = t >= 0 ? r.dir : r.dir * -1.0;
        feedback.guide = std::make_pair(r.origin - d * (4000.0 * px), r.origin + d * reach);
        return p;
    }
    return std::nullopt;
}

}  // namespace tbcad::drafting
