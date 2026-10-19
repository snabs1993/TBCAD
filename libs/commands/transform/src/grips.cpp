#include "tbcad/transform/grips.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <unordered_map>

#include "tbcad/blocks/dynamic.h"
#include "tbcad/geom/geom.h"

namespace tbcad::transform {

const char* gripModeName(GripMode mode) {
    switch (mode) {
        case GripMode::Stretch:
            return "Stretch";
        case GripMode::Move:
            return "Move";
        case GripMode::Rotate:
            return "Rotate";
        case GripMode::Scale:
            return "Scale";
        case GripMode::Mirror:
            return "Mirror";
    }
    return "Stretch";
}

GripMode nextGripMode(GripMode mode) {
    return static_cast<GripMode>((static_cast<int>(mode) + 1) % 5);
}

std::size_t gripLimit(const cmd::IHost& host) {
    if (auto s = host.setting("grips.limit")) {
        try {
            long long v = std::stoll(*s);
            if (v == 0)  // like GRIPOBJLIMIT 0: grips for any number of objects
                return std::numeric_limits<std::size_t>::max();
            if (v > 0)
                return static_cast<std::size_t>(v);
        } catch (...) {
        }
    }
    return kDefaultGripLimit;
}

std::vector<Grip> collectGrips(const Document& doc, std::span<const EntityId> ids,
                               std::size_t limit) {
    std::vector<Grip> out;
    if (ids.size() > limit)
        return out;
    out.reserve(ids.size() * 2);
    for (EntityId id : ids) {
        const Entity* e = doc.find(id);
        if (!e)
            continue;
        auto pts = geom::gripPoints(*e);
        for (std::size_t i = 0; i < pts.size(); ++i)
            out.push_back({id, static_cast<int>(i), pts[i]});
        for (const blocks::DynHandle& h : blocks::dynHandles(doc, *e))
            out.push_back({id, kDynGripBase + h.param, h.world});
    }
    return out;
}

const std::vector<Grip>& cachedGrips(const Document& doc, std::span<const EntityId> ids,
                                     std::size_t limit) {
    struct Cache {
        std::uint64_t uid = 0;
        std::uint64_t revision = 0;
        std::size_t limit = 0;
        std::vector<EntityId> ids;
        std::vector<Grip> grips;
        bool valid = false;
    };
    static Cache c;
    if (!c.valid || c.uid != doc.uid() || c.revision != doc.revision() || c.limit != limit ||
        c.ids.size() != ids.size() || !std::equal(ids.begin(), ids.end(), c.ids.begin())) {
        c.grips = collectGrips(doc, ids, limit);
        c.ids.assign(ids.begin(), ids.end());
        c.uid = doc.uid();
        c.revision = doc.revision();
        c.limit = limit;
        c.valid = true;
    }
    return c.grips;
}

const Grip* hitGrip(const std::vector<Grip>& grips, Vec2 p, double apertureMm) {
    const Grip* best = nullptr;
    double bestD = apertureMm;
    for (const Grip& g : grips) {
        // Most grips of a big selection are far away: reject on one axis before the root.
        if (std::abs(g.pos.x - p.x) > bestD || std::abs(g.pos.y - p.y) > bestD)
            continue;
        double d = distance(g.pos, p);
        if (d <= bestD) {
            bestD = d;
            best = &g;
        }
    }
    return best;
}

namespace {
std::optional<Vec2> g_hot;
}

void setHotGrip(std::optional<Vec2> pos) {
    g_hot = pos;
}

std::optional<Vec2> hotGrip() {
    return g_hot;
}

namespace {
std::vector<Vec2> g_warm;
}

void toggleWarmGrip(Vec2 pos) {
    for (auto it = g_warm.begin(); it != g_warm.end(); ++it)
        if (distance(*it, pos) <= 1e-9) {
            g_warm.erase(it);
            return;
        }
    g_warm.push_back(pos);
}

const std::vector<Vec2>& warmGrips() {
    return g_warm;
}

void clearWarmGrips() {
    g_warm.clear();
}

std::string gripClickCommand(const Document& doc, std::span<const EntityId> ids, Vec2 world,
                             double apertureMm, std::size_t limit, bool copy) {
    const Grip* g = hitGrip(cachedGrips(doc, ids, limit), world, apertureMm);
    if (!g)
        return {};
    char buf[96];
    std::snprintf(buf, sizeof buf, "GRIPEDIT %.17g,%.17g", g->pos.x, g->pos.y);
    std::string line = buf;
    if (copy)
        line += " Copy";
    return line;
}

HotGrips hotGripsOf(const std::vector<Grip>& grips, const Grip& hot,
                    const std::vector<Vec2>& warm) {
    // Hot grips: the hot one plus selected grips at the same place (coincident vertices).
    // Midpoint-style grips of other entities are not dragged along: only grips that are the
    // hot grip's own kind of point (same index class is not tracked, location is what counts).
    HotGrips set;
    std::unordered_map<std::uint64_t, std::size_t> slot;  // entity -> position in set.order
    const bool together =
        std::any_of(warm.begin(), warm.end(), [&](Vec2 w) { return distance(w, hot.pos) <= 1e-9; });
    for (const Grip& g : grips) {
        bool take = distance(g.pos, hot.pos) <= 1e-9;
        if (!take && together)
            take = std::any_of(warm.begin(), warm.end(),
                               [&](Vec2 w) { return distance(w, g.pos) <= 1e-9; });
        if (!take)
            continue;
        auto [it, fresh] = slot.try_emplace(g.entity.value, set.order.size());
        if (fresh) {
            set.order.push_back(g.entity);
            set.indices.emplace_back();
            set.from.emplace_back();
        }
        set.indices[it->second].push_back(g.index);
        set.from[it->second].push_back(g.pos);
    }
    return set;
}

std::vector<std::pair<EntityId, Entity>> stretchResults(const Document& doc, const HotGrips& set,
                                                        const Grip& hot, Vec2 delta,
                                                        std::string* error, std::size_t limit) {
    std::vector<std::pair<EntityId, Entity>> out;
    const std::size_t count = std::min(set.order.size(), limit);
    for (std::size_t k = 0; k < count; ++k) {
        const Entity* src = doc.find(set.order[k]);
        if (!src)
            continue;
        Entity cur = *src;
        bool ok = true;
        for (std::size_t j = 0; j < set.indices[k].size(); ++j) {
            const int idx = set.indices[k][j];
            const Vec2 target = set.from[k][j] + delta;
            auto moved = idx >= kDynGripBase ? blocks::dynDrag(doc, cur, idx - kDynGripBase, target)
                                             : geom::moveGrip(cur, idx, target);
            if (!moved) {
                ok = false;
                if (error && error->empty())
                    *error = moved.error().message;
                break;
            }
            cur = std::move(moved).value();
        }
        if (ok)
            out.emplace_back(set.order[k], std::move(cur));
    }
    return out;
}

std::vector<std::pair<EntityId, Entity>> stretchResults(const Document& doc,
                                                        const std::vector<Grip>& grips,
                                                        const Grip& hot, Vec2 delta,
                                                        std::string* error) {
    return stretchResults(doc, hotGripsOf(grips, hot), hot, delta, error,
                          std::numeric_limits<std::size_t>::max());
}

std::optional<Transform2> modeTransform(GripMode mode, Vec2 base, Vec2 hotPos, Vec2 p) {
    switch (mode) {
        case GripMode::Stretch:
        case GripMode::Move:
            return Transform2::translate(p - hotPos);
        case GripMode::Rotate: {
            Vec2 v = p - base;
            if (distance(p, base) < 1e-12)
                return std::nullopt;
            double a = std::atan2(v.y, v.x);
            if (distance(hotPos, base) > 1e-12)
                a -= std::atan2(hotPos.y - base.y, hotPos.x - base.x);
            return Transform2::rotate(a, base);
        }
        case GripMode::Scale: {
            double d = distance(p, base);
            if (d < 1e-12)
                return std::nullopt;
            double ref = distance(hotPos, base);
            double f = d / (ref > 1e-12 ? ref : 1.0);
            return Transform2::scale(f, f, base);
        }
        case GripMode::Mirror: {
            auto m = Transform2::mirror(base, p);
            if (!m)
                return std::nullopt;
            return m.value();
        }
    }
    return std::nullopt;
}

}  // namespace tbcad::transform
