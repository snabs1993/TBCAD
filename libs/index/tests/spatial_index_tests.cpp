#include <doctest/doctest.h>

#include <cmath>
#include <limits>

#include "../src/hilbert.h"
#include "tbcad/index/spatial_index.h"
#include "test_util.h"

using namespace tbcad;
using namespace tbcad::index;
using namespace tbcad::index::test;

namespace {

BBox2 box(double x0, double y0, double x1, double y1) {
    return BBox2{{x0, y0}, {x1, y1}};
}

EntityId id(std::uint64_t v) {
    return EntityId{v};
}

}  // namespace

TEST_CASE("hilbert keys: consecutive cells are neighbours") {
    // Take a 64 x 64 grid inside the 2^32 grid (top 6 bits of each coordinate).
    constexpr unsigned kBits = 6;
    constexpr unsigned kN = 1u << kBits;
    std::vector<std::pair<std::uint64_t, std::pair<int, int>>> cells;
    for (unsigned x = 0; x < kN; ++x)
        for (unsigned y = 0; y < kN; ++y)
            cells.push_back({detail::hilbertKey(x << (32 - kBits), y << (32 - kBits)),
                             {static_cast<int>(x), static_cast<int>(y)}});
    std::sort(cells.begin(), cells.end());
    for (std::size_t i = 1; i < cells.size(); ++i) {
        CHECK(cells[i].first != cells[i - 1].first);
        int dist = std::abs(cells[i].second.first - cells[i - 1].second.first) +
                   std::abs(cells[i].second.second - cells[i - 1].second.second);
        REQUIRE(dist == 1);
    }
}

TEST_CASE("empty index") {
    SpatialIndex idx;
    CHECK(idx.size() == 0);
    CHECK(idx.empty());
    CHECK(idx.queryIds(box(-1e9, -1e9, 1e9, 1e9)).empty());
    CHECK(idx.nearest({0, 0}, 5, 1e9).empty());
    CHECK_FALSE(idx.remove(id(1)));
    CHECK(idx.bounds().empty());
    CHECK(idx.validate());
}

TEST_CASE("insert, query, update, remove") {
    SpatialIndex idx;
    CHECK(idx.insert(id(1), box(0, 0, 10, 10)));
    CHECK(idx.insert(id(2), box(20, 0, 30, 10)));
    CHECK(idx.insert(id(3), box(5, 5, 25, 6)));
    CHECK(idx.size() == 3);
    CHECK(idx.contains(id(2)));

    auto ids = idx.queryIds(box(8, 4, 22, 7));
    std::sort(ids.begin(), ids.end());
    CHECK(ids == std::vector<EntityId>{id(1), id(2), id(3)});

    // touching counts
    CHECK(idx.queryIds(box(10, 10, 12, 12)) == std::vector<EntityId>{id(1)});
    CHECK(idx.queryIds(box(10.001, 10.001, 12, 12)).empty());

    CHECK(idx.update(id(2), box(100, 100, 101, 101)));
    CHECK(idx.queryIds(box(25, 0, 30, 10)).size() == 1);  // only id 3 (its box reaches x=25)
    CHECK(idx.queryIds(box(99, 99, 102, 102)) == std::vector<EntityId>{id(2)});
    CHECK(idx.boxOf(id(2)).min.x == 100);

    CHECK(idx.remove(id(1)));
    CHECK_FALSE(idx.remove(id(1)));
    CHECK_FALSE(idx.contains(id(1)));
    CHECK(idx.size() == 2);
    CHECK(idx.boxOf(id(1)).empty());

    // update of an unknown id inserts it
    CHECK(idx.update(id(9), box(0, 0, 1, 1)));
    CHECK(idx.contains(id(9)));
    CHECK(idx.validate());

    idx.clear();
    CHECK(idx.size() == 0);
    CHECK(idx.validate());
}

TEST_CASE("invalid input is rejected") {
    SpatialIndex idx;
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double inf = std::numeric_limits<double>::infinity();
    CHECK_FALSE(idx.insert(id(0), box(0, 0, 1, 1)));
    CHECK_FALSE(idx.insert(id(1), BBox2{}));  // empty box
    CHECK_FALSE(idx.insert(id(1), box(nan, 0, 1, 1)));
    CHECK_FALSE(idx.insert(id(1), box(0, 0, inf, 1)));
    CHECK(idx.insert(id(1), box(0, 0, 1, 1)));
    CHECK_FALSE(idx.insert(id(1), box(5, 5, 6, 6)));  // already present
    CHECK(idx.boxOf(id(1)).max.x == 1);
    CHECK_FALSE(idx.update(id(1), box(nan, 0, 1, 1)));
    CHECK_FALSE(idx.contains(id(1)));  // an unusable update drops the entry
    CHECK(idx.validate());

    std::vector<IdBox> items{{id(1), box(0, 0, 1, 1)},
                             {id(1), box(50, 50, 51, 51)},
                             {id(0), box(0, 0, 1, 1)},
                             {id(2), BBox2{}},
                             {id(3), box(2, 2, 3, 3)}};
    idx.bulkLoad(items);
    CHECK(idx.size() == 2);
    CHECK(idx.boxOf(id(1)).max.x == 1);  // first occurrence kept
    CHECK(idx.validate());
}

TEST_CASE("callbacks can stop the query early") {
    SpatialIndex idx;
    for (std::uint64_t i = 1; i <= 1000; ++i)
        idx.insert(id(i), box(double(i), 0, double(i) + 1, 1));
    int seen = 0;
    idx.query(box(0, 0, 2000, 1), [&](EntityId, const BBox2&) {
        ++seen;
        return seen < 5;
    });
    CHECK(seen == 5);
    seen = 0;
    idx.queryPoint({500, 0.5}, 1000, [&](EntityId, const BBox2&) {
        ++seen;
        return false;
    });
    CHECK(seen == 1);
}

TEST_CASE("randomised: identical to brute force through inserts, removes and updates") {
    constexpr int kBoxes = kFullScale ? 100000 : 20000;
    constexpr int kQueries = kFullScale ? 10000 : 2000;
    BoxGen gen(12345);
    SpatialIndex idx;
    FlatModel model;

    // Start from a bulk load of 70% and add the rest incrementally.
    std::vector<IdBox> bulk;
    for (int i = 1; i <= kBoxes * 7 / 10; ++i) {
        BBox2 b = gen.next();
        bulk.emplace_back(id(i), b);
        model.set(i, b);
    }
    idx.bulkLoad(bulk);
    CHECK(idx.validate());
    std::uint64_t nextId = kBoxes * 7 / 10 + 1;
    for (; nextId <= kBoxes; ++nextId) {
        BBox2 b = gen.next();
        REQUIRE(idx.insert(id(nextId), b));
        model.set(nextId, b);
    }
    // Mixed edits.
    for (int step = 0; step < kBoxes * 6 / 10; ++step) {
        double r = gen.uni(0, 1);
        std::uint64_t pick = 1 + gen.rng() % (nextId - 1);
        if (r < 0.3) {
            bool had = model.erase(pick);
            CHECK(idx.remove(id(pick)) == had);
        } else if (r < 0.7) {
            BBox2 b = gen.next();
            idx.update(id(pick), b);
            model.set(pick, b);
        } else if (r < 0.85) {
            // small move of an existing box (the common drag case)
            if (model.has(pick)) {
                BBox2 b = model.box[pick];
                double dx = gen.uni(-5, 5);
                b.min.x += dx;
                b.max.x += dx;
                idx.update(id(pick), b);
                model.set(pick, b);
            }
        } else {
            BBox2 b = gen.next();
            bool added = idx.insert(id(nextId), b);
            CHECK(added);
            model.set(nextId++, b);
        }
    }
    CHECK(idx.size() == model.count);
    CHECK(idx.validate());
    std::vector<BBox2> windows;
    for (int q = 0; q < kQueries; ++q)
        windows.push_back(gen.queryBox());
    auto want = model.queryAll(windows);
    for (int q = 0; q < kQueries; ++q)
        REQUIRE(indexQuery(idx, windows[q]) == want[q]);
    idx.rebalance();
    CHECK(idx.size() == model.count);
    CHECK(idx.validate());
    for (int q = 0; q < 200; ++q) {
        BBox2 w = gen.queryBox();
        REQUIRE(indexQuery(idx, w) == model.query(w));
    }
}

TEST_CASE("randomised: removing everything leaves a valid empty index") {
    BoxGen gen(7);
    SpatialIndex idx;
    std::vector<std::uint64_t> ids;
    for (std::uint64_t i = 1; i <= 5000; ++i) {
        idx.insert(id(i), gen.next());
        ids.push_back(i);
    }
    std::shuffle(ids.begin(), ids.end(), gen.rng);
    for (std::size_t n = 0; n < ids.size(); ++n) {
        REQUIRE(idx.remove(id(ids[n])));
        if (n % 500 == 0)
            REQUIRE(idx.validate());
    }
    CHECK(idx.size() == 0);
    CHECK(idx.validate());
    CHECK(idx.insert(id(1), box(0, 0, 1, 1)));
    CHECK(idx.queryIds(box(0, 0, 1, 1)).size() == 1);
}

TEST_CASE("many identical and zero-area boxes") {
    SpatialIndex idx;
    for (std::uint64_t i = 1; i <= 3000; ++i)
        REQUIRE(idx.insert(id(i), box(5, 5, 5, 5)));  // identical points
    for (std::uint64_t i = 3001; i <= 6000; ++i)
        REQUIRE(idx.insert(id(i), box(double(i), 7, double(i) + 100, 7)));  // horizontal lines
    CHECK(idx.validate());
    CHECK(idx.queryIds(box(5, 5, 5, 5)).size() == 3000);
    CHECK(idx.queryIds(box(4000, 7, 4000, 7)).size() == 101);
    for (std::uint64_t i = 1; i <= 3000; ++i)
        REQUIRE(idx.remove(id(i)));
    CHECK(idx.validate());
    CHECK(idx.size() == 3000);

    std::vector<IdBox> same;
    for (std::uint64_t i = 1; i <= 2000; ++i)
        same.emplace_back(id(i), box(1, 1, 1, 1));
    idx.bulkLoad(same);
    CHECK(idx.validate());
    CHECK(idx.queryIds(box(0, 0, 2, 2)).size() == 2000);
}

TEST_CASE("queryPoint uses the true distance to the box") {
    SpatialIndex idx;
    idx.insert(id(1), box(0, 0, 10, 10));
    idx.insert(id(2), box(20, 20, 30, 30));
    auto hits = [&](Vec2 p, double r) {
        std::vector<std::uint64_t> v;
        idx.queryPoint(p, r, [&](EntityId i, const BBox2&) { v.push_back(i.value); });
        std::sort(v.begin(), v.end());
        return v;
    };
    CHECK(hits({5, 5}, 0) == std::vector<std::uint64_t>{1});
    CHECK(hits({12, 5}, 2) == std::vector<std::uint64_t>{1});
    CHECK(hits({12, 5}, 1.99).empty());
    // corner: distance sqrt(2*2^2) = 2.83, inside the radius square but outside the circle
    CHECK(hits({12, 12}, 2.5).empty());
    CHECK(hits({12, 12}, 2.9) == std::vector<std::uint64_t>{1});
    CHECK(hits({15, 15}, 8) == std::vector<std::uint64_t>{1, 2});
    CHECK(hits({15, 15}, -1).empty());
}

TEST_CASE("randomised queryPoint and nearest match brute force") {
    BoxGen gen(99);
    SpatialIndex idx;
    Model model;
    std::vector<IdBox> bulk;
    for (std::uint64_t i = 1; i <= 20000; ++i) {
        BBox2 b = gen.next();
        bulk.emplace_back(id(i), b);
        model[i] = b;
    }
    idx.bulkLoad(bulk);
    for (std::uint64_t i = 20001; i <= 22000; ++i) {
        BBox2 b = gen.next();
        idx.insert(id(i), b);
        model[i] = b;
    }
    auto dist = [](Vec2 p, const BBox2& b) {
        double dx = std::max({b.min.x - p.x, 0.0, p.x - b.max.x});
        double dy = std::max({b.min.y - p.y, 0.0, p.y - b.max.y});
        return std::sqrt(dx * dx + dy * dy);
    };
    for (int q = 0; q < 300; ++q) {
        Vec2 p{gen.uni(-5e7, 5e7), gen.uni(-5e7, 5e7)};
        if (q % 2)
            p = {std::fmod(p.x, 1e4), std::fmod(p.y, 1e4)};
        double r = std::exp(gen.uni(std::log(1.0), std::log(1e6)));
        std::vector<std::uint64_t> want;
        for (const auto& [i, b] : model)
            if (dist(p, b) <= r)
                want.push_back(i);
        std::vector<std::uint64_t> got;
        idx.queryPoint(p, r, [&](EntityId i, const BBox2&) { got.push_back(i.value); });
        std::sort(got.begin(), got.end());
        REQUIRE(got == want);

        std::size_t k = 1 + gen.rng() % 20;
        std::vector<std::pair<double, std::uint64_t>> all;
        for (const auto& [i, b] : model) {
            double d = dist(p, b);
            if (d <= r)
                all.push_back({d, i});
        }
        std::sort(all.begin(), all.end());
        if (all.size() > k)
            all.resize(k);
        auto nn = idx.nearest(p, k, r);
        REQUIRE(nn.size() == all.size());
        for (std::size_t i = 0; i < nn.size(); ++i) {
            CHECK(nn[i].id.value == all[i].second);
            CHECK(nn[i].distance == doctest::Approx(all[i].first).epsilon(1e-12));
        }
    }
    CHECK(idx.nearest({0, 0}, 0, 1e9).empty());
}

TEST_CASE("large boxes live in a separate list and stay correct") {
    BoxGen gen(5);
    SpatialIndex idx;
    Model model;
    std::vector<IdBox> bulk;
    for (std::uint64_t i = 1; i <= 5000; ++i) {
        BBox2 b = box(double(i % 100) * 10, double(i / 100) * 10, double(i % 100) * 10 + 5,
                      double(i / 100) * 10 + 5);
        bulk.emplace_back(id(i), b);
        model[i] = b;
    }
    // a hatch over the whole drawing and a big region
    bulk.emplace_back(id(6001), box(-10, -10, 1100, 600));
    model[6001] = box(-10, -10, 1100, 600);
    bulk.emplace_back(id(6002), box(0, 0, 700, 400));
    model[6002] = box(0, 0, 700, 400);
    idx.bulkLoad(bulk);
    CHECK(idx.largeCount() == 2);
    CHECK(idx.validate());

    // incremental insert of another giant box
    CHECK(idx.insert(id(6003), box(-5, -5, 1000, 500)));
    model[6003] = box(-5, -5, 1000, 500);
    CHECK(idx.largeCount() == 3);
    for (int q = 0; q < 200; ++q) {
        double x = gen.uni(-20, 1100), y = gen.uni(-20, 600);
        BBox2 w = box(x, y, x + gen.uni(0, 80), y + gen.uni(0, 80));
        REQUIRE(indexQuery(idx, w) == bruteQuery(model, w));
    }
    // update moves a box between the list and the tree
    CHECK(idx.update(id(6003), box(1, 1, 2, 2)));
    model[6003] = box(1, 1, 2, 2);
    CHECK(idx.largeCount() == 2);
    CHECK(idx.update(id(5), box(-1000, -1000, 5000, 5000)));
    model[5] = box(-1000, -1000, 5000, 5000);
    CHECK(idx.largeCount() == 3);
    CHECK(idx.remove(id(6001)));
    model.erase(6001);
    CHECK(idx.largeCount() == 2);
    CHECK(idx.validate());
    CHECK(idx.size() == model.size());
    for (int q = 0; q < 200; ++q) {
        double x = gen.uni(-20, 1100), y = gen.uni(-20, 600);
        BBox2 w = box(x, y, x + gen.uni(0, 80), y + gen.uni(0, 80));
        REQUIRE(indexQuery(idx, w) == bruteQuery(model, w));
    }
    // nearest sees large boxes too
    auto nn = idx.nearest({3000, 3000}, 1, 1e9);
    REQUIRE(nn.size() == 1);
    CHECK(nn[0].id == id(5));
    CHECK(nn[0].distance == 0);
}

TEST_CASE("a mix of a 100 km site and 1 mm details") {
    SpatialIndex idx;
    std::vector<IdBox> items;
    std::uint64_t n = 1;
    for (int i = 0; i < 100; ++i)  // detail cluster, 1 mm boxes on a 2 mm pitch
        for (int j = 0; j < 100; ++j)
            items.emplace_back(id(n++), box(i * 2.0, j * 2.0, i * 2.0 + 1, j * 2.0 + 1));
    for (int i = 0; i < 100; ++i)  // sparse site 100 km away
        items.emplace_back(id(n++), box(1e8 + i * 1000.0, 1e8, 1e8 + i * 1000.0 + 500, 1e8 + 500));
    idx.bulkLoad(items);
    CHECK(idx.validate());
    CHECK(idx.queryIds(box(0, 0, 19.5, 19.5)).size() == 100);  // 10 x 10 cells
    CHECK(idx.queryIds(box(1e8, 1e8, 1e8 + 5000, 1e8 + 10)).size() == 6);
    auto nn = idx.nearest({0.4, 0.4}, 1, 1.0);
    REQUIRE(nn.size() == 1);
    CHECK(nn[0].id == id(1));
}

TEST_CASE("move keeps the index usable") {
    SpatialIndex a;
    a.insert(id(1), box(0, 0, 1, 1));
    SpatialIndex b = std::move(a);
    CHECK(b.size() == 1);
    CHECK(a.size() == 0);  // NOLINT(bugprone-use-after-move): moved-from is empty by contract
    CHECK(a.insert(id(2), box(0, 0, 1, 1)));
    a = std::move(b);
    CHECK(a.contains(id(1)));
}

TEST_CASE("bulk load leaves the id table to the first lookup, sorted ids or not") {
    for (bool sorted : {true, false}) {
        std::vector<IdBox> items;
        for (std::uint64_t i = 1; i <= 5000; ++i)
            items.emplace_back(id(sorted ? i : 5001 - i),
                               box(double(i % 100), double(i / 100), double(i % 100) + 0.5,
                                   double(i / 100) + 0.5));
        items.emplace_back(id(9000), box(-1e5, -1e5, 1e5, 1e5));  // a large box
        SpatialIndex idx;
        idx.bulkLoad(items);
        CHECK(idx.size() == items.size());
        // Queries do not need the table; every lookup below builds it on first use.
        CHECK(idx.queryIds(box(0, 0, 100, 50)).size() == 5001);
        CHECK(idx.contains(id(9000)));
        CHECK(idx.boxOf(id(9000)).max.x == 1e5);
        CHECK(idx.contains(id(1)));
        CHECK_FALSE(idx.contains(id(5001)));
        CHECK(idx.remove(id(1)));
        CHECK(idx.remove(id(9000)));
        CHECK(idx.update(id(2), box(500, 500, 501, 501)));
        CHECK(idx.insert(id(7000), box(1, 1, 2, 2)));
        CHECK(idx.size() == 5000);
        CHECK(idx.validate());
    }
}

TEST_CASE("an edit right after bulk load works without any lookup first") {
    std::vector<IdBox> items;
    for (std::uint64_t i = 1; i <= 3000; ++i)
        items.emplace_back(id(i), box(double(i), 0, double(i) + 1, 1));
    SpatialIndex idx;
    idx.bulkLoad(items);
    CHECK_FALSE(idx.insert(id(10), box(0, 0, 1, 1)));  // exists: found through the lazy table
    CHECK(idx.size() == 3000);
    CHECK(idx.validate());
}

TEST_CASE("a window over a very large index visits every entry (traversal stack depth)") {
    SpatialIndex idx;
    const int n = 600000;
    for (int i = 0; i < n; ++i) {
        const double x = (i % 800) * 10.0, y = (i / 800) * 10.0;
        REQUIRE(idx.insert(id(static_cast<std::uint64_t>(i) + 1), box(x, y, x + 5, y + 5)));
    }
    std::size_t seen = 0;
    idx.query(box(-1, -1, 1e6, 1e6), [&](EntityId, const BBox2&) { ++seen; });
    CHECK(seen == static_cast<std::size_t>(n));
}
