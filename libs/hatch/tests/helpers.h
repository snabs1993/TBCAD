#pragma once

#include <cmath>
#include <vector>

#include "tbcad/core/entities.h"
#include "tbcad/hatch/boundary.h"

namespace tbcad::hatch::testing {

inline Entity ent(EntityData d) {
    Entity e;
    e.data = std::move(d);
    return e;
}
inline Entity line(Vec2 a, Vec2 b) {
    return ent(LineData{a, b});
}
inline Entity circle(Vec2 c, double r) {
    return ent(CircleData{c, r});
}
inline Entity arc(Vec2 c, double r, double a0, double a1) {
    return ent(ArcData{c, r, a0, a1});
}
inline Entity rectPolyline(Vec2 lo, Vec2 hi) {
    PolylineData pl;
    pl.closed = true;
    pl.v = {{lo, 0, 0, 0}, {{hi.x, lo.y}, 0, 0, 0}, {hi, 0, 0, 0}, {{lo.x, hi.y}, 0, 0, 0}};
    return ent(std::move(pl));
}

// Owns entities and hands out candidates with ids 1, 2, 3 ...
class Scene {
public:
    EntityId add(Entity e) {
        items_.push_back(std::move(e));
        return EntityId{items_.size()};
    }
    void rectLines(Vec2 lo, Vec2 hi) {
        add(line(lo, {hi.x, lo.y}));
        add(line({hi.x, lo.y}, hi));
        add(line(hi, {lo.x, hi.y}));
        add(line({lo.x, hi.y}, lo));
    }
    std::vector<Candidate> candidates() const {
        std::vector<Candidate> out;
        for (std::size_t i = 0; i < items_.size(); ++i)
            out.push_back({EntityId{i + 1}, &items_[i]});
        return out;
    }
    std::size_t size() const { return items_.size(); }

private:
    std::vector<Entity> items_{};
};

}  // namespace tbcad::hatch::testing
