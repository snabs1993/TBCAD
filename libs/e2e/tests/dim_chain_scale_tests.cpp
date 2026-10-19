// DIMCONTINUE, DIMBASELINE, DIMSPACE and QDIM on a thousand objects: the numbers are right, the
// work is one undo step, and it does not take quadratic time.
#include <doctest/doctest.h>

#include <chrono>
#include <cmath>
#include <string>
#include <vector>

#include "tbcad/cmd/cmd.h"
#include "tbcad/cmd/register_all.h"
#include "tbcad/cmd/script_host.h"
#include "tbcad/dim/dim.h"

using namespace tbcad;
using cmd::ScriptItem;

namespace {

constexpr int kN = 1000;

struct Rig {
    Document doc;
    cmd::Registry reg;
    cmd::ScriptHost host;
    Rig() : host(doc, (registerAllCommands(reg), reg)) {}
    std::vector<const DimensionData*> dims() {
        std::vector<const DimensionData*> out;
        for (EntityId id : doc.modelSpace())
            if (const Entity* e = doc.find(id))
                if (const auto* d = entityAs<DimensionData>(*e))
                    out.push_back(d);
        return out;
    }
    std::size_t count() { return dims().size(); }
};

double since(std::chrono::steady_clock::time_point t) {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - t).count();
}

std::string pt(double x, double y) {
    return std::to_string(x) + "," + std::to_string(y);
}

}  // namespace

TEST_CASE("DIMCONTINUE chains a thousand dimensions end to end in one undo step") {
    Rig r;
    REQUIRE(r.host.run("DIMLINEAR", {"0,0", "10,0", "5,8"}).finished);
    std::vector<ScriptItem> items;
    for (int i = 2; i <= kN; ++i)
        items.emplace_back(pt(10.0 * i, 0));
    items.push_back(ScriptItem::enter());
    const auto t0 = std::chrono::steady_clock::now();
    REQUIRE(r.host.run("DIMCONTINUE", std::move(items)).finished);
    const double sec = since(t0);
    INFO("1000 DIMCONTINUE took " << sec << " s");
    REQUIRE(r.count() == static_cast<std::size_t>(kN));
    const auto ds = r.dims();
    for (std::size_t i = 1; i < ds.size(); ++i) {
        CHECK(distance(ds[i]->defPts[0], ds[i - 1]->defPts[1]) < 1e-9);
        CHECK(dim::measurement(*ds[i]) == doctest::Approx(10.0));
    }
    CHECK(sec < 30);
    REQUIRE(r.host.undo());
    CHECK(r.count() == 1);  // the first dimension stays, the whole chain goes in one step
    REQUIRE(r.host.redo());
    CHECK(r.count() == static_cast<std::size_t>(kN));
}

TEST_CASE("DIMBASELINE stacks a thousand dimensions from the same origin at an even spacing") {
    Rig r;
    REQUIRE(r.host.run("DIMLINEAR", {"0,0", "10,0", "5,8"}).finished);
    std::vector<ScriptItem> items;
    for (int i = 2; i <= kN; ++i)
        items.emplace_back(pt(10.0 * i, 0));
    items.push_back(ScriptItem::enter());
    const auto t0 = std::chrono::steady_clock::now();
    REQUIRE(r.host.run("DIMBASELINE", std::move(items)).finished);
    INFO("1000 DIMBASELINE took " << since(t0) << " s");
    REQUIRE(r.count() == static_cast<std::size_t>(kN));
    const auto ds = r.dims();
    const double step = ds[1]->defPts[2].y - ds[0]->defPts[2].y;
    CHECK(step > 0);
    for (std::size_t i = 1; i < ds.size(); ++i) {
        CHECK(distance(ds[i]->defPts[0], ds[0]->defPts[0]) < 1e-9);  // same origin
        CHECK(ds[i]->defPts[2].y - ds[i - 1]->defPts[2].y == doctest::Approx(step));
        CHECK(dim::measurement(*ds[i]) == doctest::Approx(10.0 * static_cast<double>(i + 1)));
    }
    CHECK(since(t0) < 30);
}

TEST_CASE("DIMSPACE evens out the spacing of a thousand parallel dimensions and undoes") {
    Rig r;
    // Dimensions at scattered heights.
    for (int i = 0; i < kN; ++i) {
        const double y = 8 + std::fmod(i * 7.31, 400.0);
        REQUIRE(r.host.run("DIMLINEAR", {"0,0", "50,0", pt(25, y)}).finished);
    }
    REQUIRE(r.count() == static_cast<std::size_t>(kN));
    std::vector<EntityId> all(r.doc.modelSpace().begin(), r.doc.modelSpace().end());
    auto heights = [&] {
        std::vector<double> h;
        for (const auto* d : r.dims())
            h.push_back(d->defPts[2].y);
        return h;
    };
    const std::vector<double> before = heights();
    const auto t0 = std::chrono::steady_clock::now();
    REQUIRE(r.host
                .run("DIMSPACE", {ScriptItem::pick(all.front(), {25, before.front()}),
                                  ScriptItem::select(all), ScriptItem::enter(), "6"})
                .finished);
    INFO("DIMSPACE on 1000 took " << since(t0) << " s");
    std::vector<double> after = heights();
    std::sort(after.begin(), after.end());
    // Every neighbouring pair is 6 apart, except where the base dimension itself sits.
    int uneven = 0;
    for (std::size_t i = 1; i < after.size(); ++i)
        uneven += std::abs(after[i] - after[i - 1] - 6) > 1e-6 ? 1 : 0;
    CHECK(uneven <= 1);
    REQUIRE(r.host.undo());
    CHECK(heights() == before);
}

TEST_CASE("QDIM dimensions the key points of a thousand objects in one chain") {
    Rig r;
    std::vector<EntityId> ids;
    {
        Transaction tx = r.doc.begin("lines");
        for (int i = 0; i < kN; ++i) {
            Entity e;
            e.data = LineData{{12.0 * i, 0}, {12.0 * i, 6}};
            ids.push_back(tx.add(std::move(e)).value());
        }
        tx.commit();
    }
    const auto t0 = std::chrono::steady_clock::now();
    REQUIRE(r.host.run("QDIM", {ScriptItem::select(ids), ScriptItem::enter(), "-10,-20"}).finished);
    INFO("QDIM on 1000 took " << since(t0) << " s");
    // One chain: a dimension between each pair of neighbouring key positions.
    CHECK(r.count() >= static_cast<std::size_t>(kN - 1));
    for (const auto* d : r.dims())
        CHECK(dim::measurement(*d) == doctest::Approx(12.0));
    CHECK(since(t0) < 30);
    const std::size_t made = r.count();
    REQUIRE(made > 0);
    REQUIRE(r.host.undo());
    CHECK(r.count() == 0);
}
