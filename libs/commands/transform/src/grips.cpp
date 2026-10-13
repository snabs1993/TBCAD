#include "tbcad/transform/grips.h"

#include <cmath>
#include <cstdio>

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
            if (v >= 0)
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

const Grip* hitGrip(const std::vector<Grip>& grips, Vec2 p, double apertureMm) {
    const Grip* best = nullptr;
    double bestD = apertureMm;
    for (const Grip& g : grips) {
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

std::string gripClickCommand(const Document& doc, std::span<const EntityId> ids, Vec2 world,
                             double apertureMm, std::size_t limit, bool copy) {
    auto grips = collectGrips(doc, ids, limit);
    const Grip* g = hitGrip(grips, world, apertureMm);
    if (!g)
        return {};
    char buf[96];
    std::snprintf(buf, sizeof buf, "GRIPEDIT %.17g,%.17g", g->pos.x, g->pos.y);
    std::string line = buf;
    if (copy)
        line += " Copy";
    return line;
}

std::vector<std::pair<EntityId, Entity>> stretchResults(const Document& doc,
                                                        const std::vector<Grip>& grips,
                                                        const Grip& hot, Vec2 delta,
                                                        std::string* error) {
    // Hot grips: the hot one plus selected grips at the same place (coincident vertices).
    // Midpoint-style grips of other entities are not dragged along: only grips that are the
    // hot grip's own kind of point (same index class is not tracked, location is what counts).
    std::vector<EntityId> order;
    std::vector<std::vector<int>> indices;
    for (const Grip& g : grips) {
        if (distance(g.pos, hot.pos) > 1e-9)
            continue;
        std::size_t k = 0;
        while (k < order.size() && order[k] != g.entity)
            ++k;
        if (k == order.size()) {
            order.push_back(g.entity);
            indices.emplace_back();
        }
        indices[k].push_back(g.index);
    }
    std::vector<std::pair<EntityId, Entity>> out;
    for (std::size_t k = 0; k < order.size(); ++k) {
        const Entity* src = doc.find(order[k]);
        if (!src)
            continue;
        Entity cur = *src;
        bool ok = true;
        for (int idx : indices[k]) {
            auto moved = idx >= kDynGripBase
                             ? blocks::dynDrag(doc, cur, idx - kDynGripBase, hot.pos + delta)
                             : geom::moveGrip(cur, idx, hot.pos + delta);
            if (!moved) {
                ok = false;
                if (error && error->empty())
                    *error = moved.error().message;
                break;
            }
            cur = std::move(moved).value();
        }
        if (ok)
            out.emplace_back(order[k], std::move(cur));
    }
    return out;
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
