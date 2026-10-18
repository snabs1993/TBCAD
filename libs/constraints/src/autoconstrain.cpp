#include <algorithm>
#include <cmath>

#include "tbcad/constraints/constraints.h"

namespace tbcad::constraints {

namespace {

struct Pt {
    Ref ref;
    Vec2 p;
};

std::vector<Pt> pointsOf(EntityId id, const Entity& e) {
    if (const auto* l = entityAs<LineData>(e))
        return {{{id, Part::Start}, l->a}, {{id, Part::End}, l->b}};
    if (const auto* c = entityAs<CircleData>(e))
        return {{{id, Part::Center}, c->center}};
    if (const auto* a = entityAs<ArcData>(e)) {
        return {{{id, Part::Center}, a->center},
                {{id, Part::Start},
                 a->center + Vec2{std::cos(a->startAngle), std::sin(a->startAngle)} * a->radius},
                {{id, Part::End},
                 a->center + Vec2{std::cos(a->endAngle), std::sin(a->endAngle)} * a->radius}};
    }
    return {};
}

bool has(const ConstraintSet& set, const std::vector<Constraint>& added, Kind k, Ref a, Ref b) {
    const auto match = [&](const Constraint& c) {
        return c.kind == k && ((c.a == a && c.b == b) || (c.a == b && c.b == a));
    };
    return std::any_of(set.all().begin(), set.all().end(), match) ||
           std::any_of(added.begin(), added.end(), match);
}

bool hasOne(const ConstraintSet& set, const std::vector<Constraint>& added, Kind k, Ref a) {
    const auto match = [&](const Constraint& c) { return c.kind == k && c.a == a; };
    return std::any_of(set.all().begin(), set.all().end(), match) ||
           std::any_of(added.begin(), added.end(), match);
}

}  // namespace

std::vector<Constraint> autoConstrain(const Document& doc, const std::vector<EntityId>& ids,
                                      const ConstraintSet& existing, const AutoOptions& opt) {
    struct Item {
        EntityId id;
        const Entity* e;
    };
    std::vector<Item> items;
    for (EntityId id : ids) {
        const Entity* e = doc.find(id);
        if (!e)
            continue;
        const EntityKind k = kind(*e);
        if (k == EntityKind::Line || k == EntityKind::Circle || k == EntityKind::Arc ||
            k == EntityKind::Point)
            items.push_back({id, e});
    }
    std::vector<Constraint> out;
    const auto add = [&](Kind k, Ref a, Ref b = {}) {
        Constraint c;
        c.kind = k;
        c.a = a;
        c.b = b;
        out.push_back(c);
    };

    // Coincident points of different objects.
    if (opt.coincident) {
        std::vector<Pt> all;
        for (const Item& it : items) {
            if (const auto* p = entityAs<PointData>(*it.e))
                all.push_back({{it.id, Part::Start}, p->p});
            else
                for (const Pt& q : pointsOf(it.id, *it.e))
                    all.push_back(q);
        }
        // One representative per cluster, so three meeting corners give two constraints, not three.
        std::vector<char> done(all.size(), 0);
        for (std::size_t i = 0; i < all.size(); ++i) {
            if (done[i])
                continue;
            for (std::size_t j = i + 1; j < all.size(); ++j) {
                if (done[j] || all[j].ref.entity == all[i].ref.entity)
                    continue;
                if (distance(all[i].p, all[j].p) > opt.distance)
                    continue;
                // The same pair (or a chain through the same cluster) may already hold.
                if (!has(existing, out, Kind::Coincident, all[i].ref, all[j].ref))
                    add(Kind::Coincident, all[i].ref, all[j].ref);
                done[j] = 1;
            }
        }
    }

    // Horizontal and vertical lines.
    if (opt.horizontalVertical) {
        for (const Item& it : items) {
            const auto* l = entityAs<LineData>(*it.e);
            if (!l)
                continue;
            const Vec2 d = l->b - l->a;
            const double len = length(d);
            if (len <= opt.distance)
                continue;
            const Ref r{it.id, Part::Whole};
            if (std::abs(d.y) <= opt.angle * len && !hasOne(existing, out, Kind::Horizontal, r))
                add(Kind::Horizontal, r);
            else if (std::abs(d.x) <= opt.angle * len && !hasOne(existing, out, Kind::Vertical, r))
                add(Kind::Vertical, r);
        }
    }

    // Parallel and perpendicular line pairs (not for pairs already fixed by horizontal/vertical).
    if (opt.parallelPerpendicular) {
        const auto orientation = [&](const LineData& l) {
            const Vec2 d = l.b - l.a;
            const double len = length(d);
            if (std::abs(d.y) <= opt.angle * len)
                return 1;  // horizontal
            if (std::abs(d.x) <= opt.angle * len)
                return 2;  // vertical
            return 0;
        };
        for (std::size_t i = 0; i < items.size(); ++i) {
            const auto* a = entityAs<LineData>(*items[i].e);
            if (!a || length(a->b - a->a) <= opt.distance)
                continue;
            for (std::size_t j = i + 1; j < items.size(); ++j) {
                const auto* b = entityAs<LineData>(*items[j].e);
                if (!b || length(b->b - b->a) <= opt.distance)
                    continue;
                const Vec2 da = normalized(a->b - a->a), db = normalized(b->b - b->a);
                const int oa = orientation(*a), ob = orientation(*b);
                const Ref ra{items[i].id, Part::Whole}, rb{items[j].id, Part::Whole};
                if (std::abs(cross(da, db)) <= opt.angle) {
                    if (oa != 0 && oa == ob)
                        continue;
                    if (!has(existing, out, Kind::Parallel, ra, rb))
                        add(Kind::Parallel, ra, rb);
                } else if (std::abs(dot(da, db)) <= opt.angle) {
                    if (oa != 0 && ob != 0)
                        continue;
                    if (!has(existing, out, Kind::Perpendicular, ra, rb))
                        add(Kind::Perpendicular, ra, rb);
                }
            }
        }
    }

    // Equal lengths and radii: each group chains to its first member.
    if (opt.equal) {
        std::vector<char> done(items.size(), 0);
        for (std::size_t i = 0; i < items.size(); ++i) {
            if (done[i])
                continue;
            const auto measure = [&](const Entity& e) -> std::optional<std::pair<int, double>> {
                if (const auto* l = entityAs<LineData>(e))
                    return std::pair{0, length(l->b - l->a)};
                if (const auto* c = entityAs<CircleData>(e))
                    return std::pair{1, c->radius};
                if (const auto* a = entityAs<ArcData>(e))
                    return std::pair{1, a->radius};
                return std::nullopt;
            };
            const auto mi = measure(*items[i].e);
            if (!mi || mi->second <= opt.distance)
                continue;
            for (std::size_t j = i + 1; j < items.size(); ++j) {
                if (done[j])
                    continue;
                const auto mj = measure(*items[j].e);
                if (!mj || mj->first != mi->first ||
                    std::abs(mj->second - mi->second) > opt.distance)
                    continue;
                const Ref ra{items[i].id, Part::Whole}, rb{items[j].id, Part::Whole};
                if (!has(existing, out, Kind::Equal, ra, rb))
                    add(Kind::Equal, ra, rb);
                done[j] = 1;
            }
        }
    }

    // Tangent line/circle and circle/circle.
    if (opt.tangent) {
        for (std::size_t i = 0; i < items.size(); ++i) {
            for (std::size_t j = i + 1; j < items.size(); ++j) {
                const auto circ = [](const Entity& e) -> std::optional<std::pair<Vec2, double>> {
                    if (const auto* c = entityAs<CircleData>(e))
                        return std::pair{c->center, c->radius};
                    if (const auto* a = entityAs<ArcData>(e))
                        return std::pair{a->center, a->radius};
                    return std::nullopt;
                };
                const Entity &ea = *items[i].e, &eb = *items[j].e;
                const Ref ra{items[i].id, Part::Whole}, rb{items[j].id, Part::Whole};
                bool tangent = false;
                const auto ca = circ(ea), cb = circ(eb);
                const auto* la = entityAs<LineData>(ea);
                const auto* lb = entityAs<LineData>(eb);
                if (ca && cb) {
                    const double d = distance(ca->first, cb->first);
                    tangent = std::abs(d - (ca->second + cb->second)) <= opt.distance ||
                              (d > opt.distance &&
                               std::abs(d - std::abs(ca->second - cb->second)) <= opt.distance);
                } else if ((la && cb) || (lb && ca)) {
                    const LineData* l = la ? la : lb;
                    const auto c = la ? cb : ca;
                    const Vec2 d = l->b - l->a;
                    if (length(d) > opt.distance) {
                        const double off = std::abs(cross(normalized(d), c->first - l->a));
                        tangent = std::abs(off - c->second) <= opt.distance;
                    }
                }
                if (tangent && !has(existing, out, Kind::Tangent, ra, rb))
                    add(Kind::Tangent, ra, rb);
            }
        }
    }
    return out;
}

}  // namespace tbcad::constraints
