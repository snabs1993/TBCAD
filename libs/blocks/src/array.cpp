#include "tbcad/blocks/array.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <map>

#include "tbcad/geom/geom.h"

namespace tbcad::blocks {

namespace {
std::string num(double v) {
    char buf[40];
    std::snprintf(buf, sizeof buf, "%.17g", v);
    return buf;
}
}  // namespace

std::string arrayToText(const ArrayInfo& a) {
    std::string out = "kind=" + std::to_string(static_cast<int>(a.kind)) +
                      ",rows=" + std::to_string(a.rows) + ",cols=" + std::to_string(a.columns) +
                      ",rs=" + num(a.rowSpacing) + ",cs=" + num(a.columnSpacing) +
                      ",ang=" + num(a.angle) + ",cx=" + num(a.center.x) + ",cy=" + num(a.center.y) +
                      ",count=" + std::to_string(a.count) + ",fill=" + num(a.fillAngle) +
                      ",rot=" + (a.rotateItems ? "1" : "0") + ",rx=" + num(a.reference.x) +
                      ",ry=" + num(a.reference.y) + ",pc=" + (a.pathClosed ? "1" : "0") +
                      ",ps=" + num(a.pathSpacing) + ",path=";
    for (std::size_t i = 0; i < a.path.size(); ++i) {
        if (i)
            out += ';';
        out += num(a.path[i].p.x) + ":" + num(a.path[i].p.y) + ":" + num(a.path[i].bulge);
    }
    if (!a.removed.empty()) {
        out += ",rm=";
        for (std::size_t i = 0; i < a.removed.size(); ++i)
            out += (i ? ";" : "") + std::to_string(a.removed[i]);
    }
    if (!a.swaps.empty()) {
        out += ",sw=";
        for (std::size_t i = 0; i < a.swaps.size(); ++i)
            out += (i ? ";" : "") + std::to_string(a.swaps[i].item) + ":" + num(a.swaps[i].at.x) +
                   ":" + num(a.swaps[i].at.y);
    }
    return out;
}

std::optional<ArrayInfo> arrayFromText(std::string_view text, double k) {
    std::map<std::string, std::string> kv;
    std::size_t at = 0;
    while (at < text.size()) {
        std::size_t comma = text.find(',', at);
        if (comma == std::string_view::npos)
            comma = text.size();
        std::string_view item = text.substr(at, comma - at);
        std::size_t eq = item.find('=');
        if (eq == std::string_view::npos)
            return std::nullopt;
        kv[std::string(item.substr(0, eq))] = std::string(item.substr(eq + 1));
        at = comma + 1;
    }
    auto real = [&](const char* key, double& v) {
        auto it = kv.find(key);
        if (it == kv.end())
            return false;
        char* end = nullptr;
        v = std::strtod(it->second.c_str(), &end);
        return end != it->second.c_str() && std::isfinite(v);
    };
    ArrayInfo a;
    double v = 0;
    if (!real("kind", v) || v < 0 || v > 2)
        return std::nullopt;
    a.kind = static_cast<ArrayInfo::Kind>(static_cast<int>(v));
    if (!real("rows", v))
        return std::nullopt;
    a.rows = static_cast<int>(v);
    if (!real("cols", v))
        return std::nullopt;
    a.columns = static_cast<int>(v);
    if (!real("count", v))
        return std::nullopt;
    a.count = static_cast<int>(v);
    if (a.rows < 1 || a.columns < 1 || a.count < 1 ||
        static_cast<long long>(a.rows) * a.columns > kMaxArrayItems || a.count > kMaxArrayItems)
        return std::nullopt;
    if (real("rs", v))
        a.rowSpacing = v * k;
    if (real("cs", v))
        a.columnSpacing = v * k;
    if (real("ang", v))
        a.angle = v;
    if (real("cx", v))
        a.center.x = v * k;
    if (real("cy", v))
        a.center.y = v * k;
    if (real("fill", v))
        a.fillAngle = v;
    if (real("rot", v))
        a.rotateItems = v != 0;
    if (real("rx", v))
        a.reference.x = v * k;
    if (real("ry", v))
        a.reference.y = v * k;
    if (real("pc", v))
        a.pathClosed = v != 0;
    if (real("ps", v))
        a.pathSpacing = v * k;
    const std::string& path = kv["path"];
    std::size_t p = 0;
    while (p < path.size()) {
        std::size_t semi = path.find(';', p);
        if (semi == std::string::npos)
            semi = path.size();
        double x = 0, y = 0, b = 0;
        if (std::sscanf(path.substr(p, semi - p).c_str(), "%lf:%lf:%lf", &x, &y, &b) != 3)
            return std::nullopt;
        PolyVertex pv;
        pv.p = {x * k, y * k};
        pv.bulge = b;
        a.path.push_back(pv);
        p = semi + 1;
    }
    if (a.kind == ArrayInfo::Kind::Path && a.path.size() < 2)
        return std::nullopt;
    const long long items = arrayItemCount(a);
    auto parts = [](const std::string& list) {
        std::vector<std::string> out;
        std::size_t q = 0;
        while (q < list.size()) {
            std::size_t semi = list.find(';', q);
            if (semi == std::string::npos)
                semi = list.size();
            out.push_back(list.substr(q, semi - q));
            q = semi + 1;
        }
        return out;
    };
    if (auto it = kv.find("rm"); it != kv.end())
        for (const std::string& part : parts(it->second)) {
            int i = -1;
            if (std::sscanf(part.c_str(), "%d", &i) != 1 || !removeArrayItem(a, i))
                return std::nullopt;
        }
    if (auto it = kv.find("sw"); it != kv.end())
        for (const std::string& part : parts(it->second)) {
            ArrayItemSwap sw;
            double x = 0, y = 0;
            if (std::sscanf(part.c_str(), "%d:%lf:%lf", &sw.item, &x, &y) != 3 || sw.item < 0 ||
                sw.item >= items)
                return std::nullopt;
            sw.at = {x * k, y * k};
            (void)swapArrayItem(a, sw);
        }
    return a;
}

bool arrayItemShowsSource(const ArrayInfo& a, int item) {
    return !std::binary_search(a.removed.begin(), a.removed.end(), item) && !arrayItemSwap(a, item);
}

const ArrayItemSwap* arrayItemSwap(const ArrayInfo& a, int item) {
    auto it = std::lower_bound(a.swaps.begin(), a.swaps.end(), item,
                               [](const ArrayItemSwap& s, int i) { return s.item < i; });
    return it != a.swaps.end() && it->item == item ? &*it : nullptr;
}

bool removeArrayItem(ArrayInfo& a, int item) {
    if (item < 0 || item >= arrayItemCount(a))
        return false;
    std::erase_if(a.swaps, [&](const ArrayItemSwap& s) { return s.item == item; });
    auto it = std::lower_bound(a.removed.begin(), a.removed.end(), item);
    if (it != a.removed.end() && *it == item)
        return false;
    a.removed.insert(it, item);
    return true;
}

bool swapArrayItem(ArrayInfo& a, ArrayItemSwap swap) {
    if (swap.item < 0 || swap.item >= arrayItemCount(a))
        return false;
    std::erase(a.removed, swap.item);
    auto it = std::lower_bound(a.swaps.begin(), a.swaps.end(), swap.item,
                               [](const ArrayItemSwap& s, int i) { return s.item < i; });
    if (it != a.swaps.end() && it->item == swap.item)
        *it = swap;
    else
        a.swaps.insert(it, swap);
    return true;
}

std::vector<Transform2> rectangularTransforms(const RectArray& s) {
    std::vector<Transform2> out;
    const Vec2 ex{std::cos(s.angle), std::sin(s.angle)};
    const Vec2 ey = perp(ex);
    for (int r = 0; r < s.rows; ++r)
        for (int c = 0; c < s.columns; ++c) {
            if (r == 0 && c == 0)
                continue;
            out.push_back(
                Transform2::translate(ex * (c * s.columnSpacing) + ey * (r * s.rowSpacing)));
        }
    return out;
}

std::vector<Transform2> polarTransforms(const PolarArray& s, Vec2 reference) {
    std::vector<Transform2> out;
    if (s.count < 2)
        return out;
    const bool full = std::abs(std::abs(s.fillAngle) - kTwoPi) < 1e-9;
    const double step = full ? s.fillAngle / s.count : s.fillAngle / (s.count - 1);
    for (int i = 1; i < s.count; ++i) {
        Transform2 rot = Transform2::rotate(step * i, s.center);
        out.push_back(s.rotateItems ? rot
                                    : Transform2::translate(rot.apply(reference) - reference));
    }
    return out;
}

namespace {

Vec2 sourceCenter(const std::vector<Entity>& source) {
    BBox2 box;
    for (const Entity& e : source)
        box.merge(geom::bounds(e));
    return box.center();
}

Result<void> checkCount(long long n) {
    if (n > kMaxArrayItems)
        return makeError(ErrorCode::OutOfRange, "That array would create more than " +
                                                    std::to_string(kMaxArrayItems) + " objects");
    return {};
}

std::vector<Entity> apply(const std::vector<Entity>& source, const std::vector<Transform2>& ts,
                          Result<void>& status) {
    std::vector<Entity> out;
    out.reserve(source.size() * ts.size());
    for (const Transform2& t : ts)
        for (const Entity& e : source) {
            auto r = geom::transformed(e, t);
            if (!r) {
                status = r.error();
                return {};
            }
            Entity copy = std::move(r).value();
            copy.common.id = {};
            out.push_back(std::move(copy));
        }
    return out;
}

}  // namespace

Result<std::vector<Entity>> arrayRectangular(const std::vector<Entity>& source,
                                             const RectArray& spec) {
    if (spec.rows < 1 || spec.columns < 1)
        return makeError(ErrorCode::InvalidArgument,
                         "An array needs at least one row and one column");
    if (auto c = checkCount(static_cast<long long>(spec.rows) * spec.columns * source.size());
        !c.ok())
        return c.error();
    Result<void> status{};
    auto out = apply(source, rectangularTransforms(spec), status);
    if (!status.ok())
        return status.error();
    return out;
}

Result<std::vector<Entity>> arrayPolar(const std::vector<Entity>& source, const PolarArray& spec) {
    if (spec.count < 1)
        return makeError(ErrorCode::InvalidArgument, "A polar array needs at least one item");
    if (!(std::abs(spec.fillAngle) > 0))
        return makeError(ErrorCode::InvalidArgument, "The angle to fill cannot be zero");
    if (auto c = checkCount(static_cast<long long>(spec.count) * source.size()); !c.ok())
        return c.error();
    Vec2 ref = spec.basePoint ? *spec.basePoint : sourceCenter(source);
    Result<void> status{};
    auto out = apply(source, polarTransforms(spec, ref), status);
    if (!status.ok())
        return status.error();
    return out;
}

namespace {

// Position and direction of travel at arc length `s` along the path segments.
struct PathPoint {
    Vec2 p{};
    double angle = 0;
};

PathPoint pathPointAt(const std::vector<geom::Segment>& segs, const std::vector<double>& lens,
                      double s) {
    for (std::size_t i = 0; i < segs.size(); ++i) {
        if (s <= lens[i] || i + 1 == segs.size()) {
            const geom::Segment& g = segs[i];
            double len = g.curve.length();
            double u = len > 0 ? std::clamp(s / len, 0.0, 1.0) : 0.0;
            double t = g.toCurveParam(u);
            Vec2 dir = g.curve.tangentAt(t);
            if (g.reversed)
                dir = dir * -1.0;
            return {g.curve.pointAt(t), std::atan2(dir.y, dir.x)};
        }
        s -= lens[i];
    }
    return {};
}

}  // namespace

long long arrayItemCount(const ArrayInfo& a) {
    switch (a.kind) {
        case ArrayInfo::Kind::Rectangular:
            return static_cast<long long>(std::max(a.rows, 1)) * std::max(a.columns, 1);
        case ArrayInfo::Kind::Polar:
        case ArrayInfo::Kind::Path:
            return std::max(a.count, 1);
    }
    return 1;
}

std::vector<Transform2> arrayItemTransforms(const ArrayInfo& a) {
    std::vector<Transform2> out;
    long long n = std::min(arrayItemCount(a), kMaxArrayItems);
    out.reserve(static_cast<std::size_t>(n));
    switch (a.kind) {
        case ArrayInfo::Kind::Rectangular: {
            const Vec2 ex{std::cos(a.angle), std::sin(a.angle)};
            const Vec2 ey = perp(ex);
            for (int r = 0; r < std::max(a.rows, 1) && static_cast<long long>(out.size()) < n; ++r)
                for (int c = 0;
                     c < std::max(a.columns, 1) && static_cast<long long>(out.size()) < n; ++c)
                    out.push_back(Transform2::translate(ex * (c * a.columnSpacing) +
                                                        ey * (r * a.rowSpacing)));
            break;
        }
        case ArrayInfo::Kind::Polar: {
            const bool full = std::abs(std::abs(a.fillAngle) - kTwoPi) < 1e-9;
            const int cnt = std::max(a.count, 1);
            const double step = cnt < 2 ? 0 : (full ? a.fillAngle / cnt : a.fillAngle / (cnt - 1));
            for (long long i = 0; i < n; ++i) {
                Transform2 rot = Transform2::rotate(step * static_cast<double>(i), a.center);
                out.push_back(a.rotateItems
                                  ? rot
                                  : Transform2::translate(rot.apply(a.reference) - a.reference));
            }
            break;
        }
        case ArrayInfo::Kind::Path: {
            PolylineData pl;
            pl.v = a.path;
            pl.closed = a.pathClosed;
            auto segs = geom::segments(pl);
            std::vector<double> lens;
            double total = 0;
            for (const auto& g : segs) {
                lens.push_back(g.curve.length());
                total += lens.back();
            }
            if (segs.empty()) {
                out.assign(static_cast<std::size_t>(n), Transform2::identity());
                break;
            }
            const int cnt = std::max(a.count, 1);
            double spacing = a.pathSpacing;
            if (!(spacing > 0))
                spacing = a.pathClosed ? total / cnt : (cnt > 1 ? total / (cnt - 1) : 0);
            const PathPoint first = pathPointAt(segs, lens, 0);
            for (long long i = 0; i < n; ++i) {
                double s = spacing * static_cast<double>(i);
                if (a.pathClosed && total > 0)
                    s = std::fmod(s, total);
                else
                    s = std::min(s, total);
                PathPoint pp = pathPointAt(segs, lens, s);
                Transform2 t = Transform2::translate(pp.p - a.reference);
                if (a.rotateItems)
                    t = Transform2::rotate(pp.angle - first.angle, pp.p) *
                        Transform2::translate(pp.p - a.reference);
                out.push_back(t);
            }
            break;
        }
    }
    return out;
}

}  // namespace tbcad::blocks
