#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <unordered_map>
#include <unordered_set>

#include "tbcad/constraints/constraints.h"
#include "tbcad/geom/box_grid.h"

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

// The constraints already there or already proposed, so each pair is offered once. A hash set:
// a scan of the list for every candidate pair made thousands of objects take seconds.
class Known {
public:
    explicit Known(const ConstraintSet& set) {
        for (const Constraint& c : set.all()) {
            pairs_.insert(key(c.kind, c.a, c.b));
            pairs_.insert(key(c.kind, c.b, c.a));
            ones_.insert(key(c.kind, c.a, Ref{}));
        }
    }
    bool has(Kind k, Ref a, Ref b) const { return pairs_.count(key(k, a, b)) != 0; }
    bool hasOne(Kind k, Ref a) const { return ones_.count(key(k, a, Ref{})) != 0; }
    void add(const Constraint& c) {
        pairs_.insert(key(c.kind, c.a, c.b));
        pairs_.insert(key(c.kind, c.b, c.a));
        ones_.insert(key(c.kind, c.a, Ref{}));
    }

private:
    struct Key {
        std::uint64_t ea, eb;
        std::uint32_t ia, ib;
        std::uint8_t kind, pa, pb;
        friend bool operator==(const Key&, const Key&) = default;
    };
    struct Hash {
        std::size_t operator()(const Key& k) const {
            std::uint64_t h = k.ea * 0x9E3779B97F4A7C15ull ^ (k.eb + 0x632BE59BD9B4E019ull + (h0(k)));
            return static_cast<std::size_t>(h ^ (h >> 29));
        }
        static std::uint64_t h0(const Key& k) {
            return (std::uint64_t{k.kind} << 56) ^ (std::uint64_t{k.pa} << 48) ^
                   (std::uint64_t{k.pb} << 40) ^ (std::uint64_t{k.ia} << 20) ^ k.ib;
        }
    };
    static Key key(Kind k, Ref a, Ref b) {
        return {a.entity.value, b.entity.value, a.index, b.index, static_cast<std::uint8_t>(k),
                static_cast<std::uint8_t>(a.part), static_cast<std::uint8_t>(b.part)};
    }
    std::unordered_set<Key, Hash> pairs_, ones_;
};

// Cell of a uniform grid, packed in one integer; coordinates far out are clamped (they only lose
// speed, not results, since every candidate is tested exactly afterwards).
std::uint64_t cellKey(double x, double y, double cell) {
    const auto c = [&](double v) {
        const double q = std::floor(v / cell);
        return static_cast<std::int64_t>(std::clamp(q, -1.0e9, 1.0e9));
    };
    return (static_cast<std::uint64_t>(c(x)) << 32) ^ (static_cast<std::uint64_t>(c(y)) & 0xFFFFFFFFull);
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
    Known known(existing);
    const auto add = [&](Kind k, Ref a, Ref b = {}) {
        Constraint c;
        c.kind = k;
        c.a = a;
        c.b = b;
        known.add(c);
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
        // Candidates come from a grid of cells as wide as the tolerance; they are taken in the
        // same order as a test of every pair would take them.
        const double cell = std::max(opt.distance, 1e-9);
        std::unordered_map<std::uint64_t, std::vector<std::size_t>> grid;
        grid.reserve(all.size());
        for (std::size_t i = 0; i < all.size(); ++i)
            grid[cellKey(all[i].p.x, all[i].p.y, cell)].push_back(i);
        std::vector<char> done(all.size(), 0);
        std::vector<std::size_t> near;
        for (std::size_t i = 0; i < all.size(); ++i) {
            if (done[i])
                continue;
            near.clear();
            for (int dy = -1; dy <= 1; ++dy)
                for (int dx = -1; dx <= 1; ++dx) {
                    auto it = grid.find(cellKey(all[i].p.x + dx * cell, all[i].p.y + dy * cell, cell));
                    if (it == grid.end())
                        continue;
                    for (std::size_t j : it->second)
                        if (j > i)
                            near.push_back(j);
                }
            std::sort(near.begin(), near.end());
            near.erase(std::unique(near.begin(), near.end()), near.end());
            for (std::size_t j : near) {
                if (done[j] || all[j].ref.entity == all[i].ref.entity)
                    continue;
                if (distance(all[i].p, all[j].p) > opt.distance)
                    continue;
                // The same pair (or a chain through the same cluster) may already hold.
                if (!known.has(Kind::Coincident, all[i].ref, all[j].ref))
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
            if (std::abs(d.y) <= opt.angle * len && !known.hasOne(Kind::Horizontal, r))
                add(Kind::Horizontal, r);
            else if (std::abs(d.x) <= opt.angle * len && !known.hasOne(Kind::Vertical, r))
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
        // Directions (0 to pi) in buckets as wide as the angle tolerance: a pair can only be
        // parallel when their buckets are neighbours, perpendicular when they are a quarter turn
        // apart. Two lines that are both horizontal (or both vertical) are skipped anyway, so a
        // horizontal line only looks for tilted lines and, when the tolerance is wide enough for
        // that, vertical ones that are still parallel to it.
        struct Dir {
            std::size_t item;
            double theta;
            int orient;
        };
        std::vector<Dir> lines;
        for (std::size_t i = 0; i < items.size(); ++i) {
            const auto* l = entityAs<LineData>(*items[i].e);
            if (!l || length(l->b - l->a) <= opt.distance)
                continue;
            double t = std::atan2(l->b.y - l->a.y, l->b.x - l->a.x);
            if (t < 0)
                t += kPi;
            if (t >= kPi)
                t -= kPi;
            lines.push_back({i, t, orientation(*l)});
        }
        const double halfWidth = std::asin(std::clamp(opt.angle, 0.0, 1.0)) + 1e-9;
        const long nb = std::clamp(static_cast<long>(kPi / std::max(halfWidth, 1e-6)), 1L, 1L << 20);
        const auto bucketOf = [&](double t) {
            long b = static_cast<long>(std::floor(t / kPi * nb)) % nb;
            return b < 0 ? b + nb : b;
        };
        // Indexes into `lines` by bucket: tilted, horizontal, vertical.
        std::array<std::vector<std::vector<std::size_t>>, 3> byOrient;
        for (auto& v : byOrient)
            v.resize(static_cast<std::size_t>(nb));
        for (std::size_t k = 0; k < lines.size(); ++k)
            byOrient[static_cast<std::size_t>(lines[k].orient)][bucketOf(lines[k].theta)].push_back(k);
        std::vector<std::size_t> cand;
        for (std::size_t k = 0; k < lines.size(); ++k) {
            const Dir& di = lines[k];
            cand.clear();
            for (const bool parallelWindow : {true, false}) {
                const double centre = parallelWindow ? di.theta : di.theta + kPi / 2;
                const long b0 = bucketOf(centre - halfWidth) - 1, b1 = b0 + 2 + static_cast<long>(
                                                                         std::ceil(2 * halfWidth / kPi * nb));
                for (long b = b0; b <= std::min(b1, b0 + nb - 1); ++b) {
                    const long w = ((b % nb) + nb) % nb;
                    for (int o = 0; o < 3; ++o) {
                        if (o != 0 && di.orient != 0 && (!parallelWindow || o == di.orient))
                            continue;
                        for (std::size_t m : byOrient[static_cast<std::size_t>(o)][w])
                            if (lines[m].item > di.item)
                                cand.push_back(m);
                    }
                }
            }
            std::sort(cand.begin(), cand.end());
            cand.erase(std::unique(cand.begin(), cand.end()), cand.end());
            const std::size_t i = di.item;
            const auto* a = entityAs<LineData>(*items[i].e);
            for (std::size_t m : cand) {
                const std::size_t j = lines[m].item;
                const auto* b = entityAs<LineData>(*items[j].e);
                const Vec2 da = normalized(a->b - a->a), db = normalized(b->b - b->a);
                const int oa = di.orient, ob = lines[m].orient;
                const Ref ra{items[i].id, Part::Whole}, rb{items[j].id, Part::Whole};
                if (std::abs(cross(da, db)) <= opt.angle) {
                    if (oa != 0 && oa == ob)
                        continue;
                    if (!known.has(Kind::Parallel, ra, rb))
                        add(Kind::Parallel, ra, rb);
                } else if (std::abs(dot(da, db)) <= opt.angle) {
                    if (oa != 0 && ob != 0)
                        continue;
                    if (!known.has(Kind::Perpendicular, ra, rb))
                        add(Kind::Perpendicular, ra, rb);
                }
            }
        }
    }

    // Equal lengths and radii: each group chains to its first member. The measures are sorted
    // so each object only looks at those within the tolerance of its own.
    if (opt.equal) {
        const auto measure = [&](const Entity& e) -> std::optional<std::pair<int, double>> {
            if (const auto* l = entityAs<LineData>(e))
                return std::pair{0, length(l->b - l->a)};
            if (const auto* c = entityAs<CircleData>(e))
                return std::pair{1, c->radius};
            if (const auto* a = entityAs<ArcData>(e))
                return std::pair{1, a->radius};
            return std::nullopt;
        };
        struct M {
            int type;
            double value;
            std::size_t item;
        };
        std::vector<M> ms;
        for (std::size_t i = 0; i < items.size(); ++i)
            if (const auto m = measure(*items[i].e))
                ms.push_back({m->first, m->second, i});
        std::vector<M> sorted = ms;
        std::sort(sorted.begin(), sorted.end(), [](const M& x, const M& y) {
            return x.type != y.type ? x.type < y.type : x.value < y.value;
        });
        std::vector<char> done(items.size(), 0);
        std::vector<std::size_t> cand;
        for (const M& mi : ms) {
            const std::size_t i = mi.item;
            if (done[i] || mi.value <= opt.distance)
                continue;
            const auto lo = std::lower_bound(
                sorted.begin(), sorted.end(), mi, [&](const M& x, const M& key) {
                    return x.type != key.type ? x.type < key.type : x.value < key.value - opt.distance;
                });
            cand.clear();
            for (auto it = lo; it != sorted.end() && it->type == mi.type &&
                               it->value <= mi.value + opt.distance;
                 ++it)
                if (it->item > i && !done[it->item] &&
                    std::abs(it->value - mi.value) <= opt.distance)
                    cand.push_back(it->item);
            std::sort(cand.begin(), cand.end());
            for (std::size_t j : cand) {
                const Ref ra{items[i].id, Part::Whole}, rb{items[j].id, Part::Whole};
                if (!known.has(Kind::Equal, ra, rb))
                    add(Kind::Equal, ra, rb);
                done[j] = 1;
            }
        }
    }

    // Tangent line/circle and circle/circle. Circle pairs come from a grid of their boxes (two
    // tangent circles touch, so their boxes meet); a line counts by its whole infinite extension,
    // so it is tried against every circle.
    if (opt.tangent) {
        const auto circ = [](const Entity& e) -> std::optional<std::pair<Vec2, double>> {
            if (const auto* c = entityAs<CircleData>(e))
                return std::pair{c->center, c->radius};
            if (const auto* a = entityAs<ArcData>(e))
                return std::pair{a->center, a->radius};
            return std::nullopt;
        };
        std::vector<std::size_t> circles, lines;
        for (std::size_t i = 0; i < items.size(); ++i) {
            if (circ(*items[i].e))
                circles.push_back(i);
            else if (entityAs<LineData>(*items[i].e))
                lines.push_back(i);
        }
        std::vector<std::pair<std::size_t, std::size_t>> pairs;
        std::vector<BBox2> boxes;
        for (std::size_t i : circles) {
            const auto c = *circ(*items[i].e);
            const double r = std::abs(c.second) + opt.distance;
            boxes.push_back(BBox2{c.first - Vec2{r, r}, c.first + Vec2{r, r}});
        }
        const geom::BoxGrid grid(boxes);
        for (std::size_t k = 0; k < circles.size(); ++k) {
            const auto ca = *circ(*items[circles[k]].e);
            grid.query(boxes[k], [&](std::size_t m) {
                if (circles[m] <= circles[k])
                    return;
                const auto cb = *circ(*items[circles[m]].e);
                const double d = distance(ca.first, cb.first);
                if (std::abs(d - (ca.second + cb.second)) <= opt.distance ||
                    (d > opt.distance &&
                     std::abs(d - std::abs(ca.second - cb.second)) <= opt.distance))
                    pairs.push_back({circles[k], circles[m]});
            });
        }
        for (std::size_t li : lines) {
            const auto* l = entityAs<LineData>(*items[li].e);
            const Vec2 d = l->b - l->a;
            if (length(d) <= opt.distance)
                continue;
            for (std::size_t ci : circles) {
                const auto c = *circ(*items[ci].e);
                const double off = std::abs(cross(normalized(d), c.first - l->a));
                if (std::abs(off - c.second) <= opt.distance)
                    pairs.push_back({std::min(li, ci), std::max(li, ci)});
            }
        }
        std::sort(pairs.begin(), pairs.end());
        for (const auto& [i, j] : pairs) {
            const Ref ra{items[i].id, Part::Whole}, rb{items[j].id, Part::Whole};
            if (!known.has(Kind::Tangent, ra, rb))
                add(Kind::Tangent, ra, rb);
        }
    }
    return out;
}

}  // namespace tbcad::constraints
