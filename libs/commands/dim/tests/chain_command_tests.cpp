#include <doctest/doctest.h>

#include <cmath>

#include "tbcad/cmd/cmd.h"
#include "tbcad/cmd/script_host.h"
#include "tbcad/dimcmd/dimcmd.h"

using namespace tbcad;
using namespace tbcad::cmd;

namespace {

struct Chain {
    Document doc;
    Registry reg;
    std::unique_ptr<ScriptHost> host;

    Chain() {
        registerBuiltinCommands(reg);
        dimcmd::registerCommands(reg);
        host = std::make_unique<ScriptHost>(doc, reg);
    }
    std::vector<const DimensionData*> dims() const {
        std::vector<const DimensionData*> out;
        doc.forEachEntity([&](const Entity& e) {
            if (const auto* d = entityAs<DimensionData>(e))
                out.push_back(d);
        });
        return out;
    }
};

}  // namespace

TEST_CASE("DIMCONTINUE chains from the second extension line, same dimension line") {
    Chain f;
    REQUIRE(f.host->run("DIMLINEAR", {"0,0", "100,0", "50,-30"}).finished);
    auto r = f.host->run("DIMCONTINUE", {"250,0", "400,0", ScriptItem::enter()});
    CHECK(r.finished);
    auto dims = f.dims();
    REQUIRE(dims.size() == 3);
    CHECK(dims[1]->defPts[0].x == doctest::Approx(100));
    CHECK(dims[1]->measurement == doctest::Approx(150));
    CHECK(dims[2]->defPts[0].x == doctest::Approx(250));
    CHECK(dims[2]->measurement == doctest::Approx(150));
    CHECK(dims[2]->defPts[2].y == doctest::Approx(-30));
    CHECK(f.host->undo());
    CHECK(f.dims().size() == 1);
}

TEST_CASE("DIMBASELINE stacks dimensions from the first extension line") {
    Chain f;
    REQUIRE(f.host->run("DIMLINEAR", {"0,0", "100,0", "50,-30"}).finished);
    auto r = f.host->run("DIMBASELINE", {"250,0", "400,0", ScriptItem::enter()});
    CHECK(r.finished);
    auto dims = f.dims();
    REQUIRE(dims.size() == 3);
    CHECK(dims[1]->defPts[0].x == doctest::Approx(0));
    CHECK(dims[1]->measurement == doctest::Approx(250));
    CHECK(dims[2]->measurement == doctest::Approx(400));
    // each one sits further out on the same side
    CHECK(dims[1]->defPts[2].y < dims[0]->defPts[2].y);
    CHECK(dims[2]->defPts[2].y < dims[1]->defPts[2].y);
}

TEST_CASE("DIMCONTINUE without a dimension asks for one") {
    Chain f;
    auto r = f.host->run("DIMCONTINUE", {ScriptItem::esc()});
    CHECK(r.finished);
    CHECK(f.dims().empty());
}

TEST_CASE("DIMTEDIT moves the text and Home puts it back") {
    Chain f;
    REQUIRE(f.host->run("DIMLINEAR", {"0,0", "100,0", "50,-30"}).finished);
    EntityId id{};
    f.doc.forEachEntity([&](const Entity& e) {
        if (kind(e) == EntityKind::Dimension)
            id = e.common.id;
    });
    REQUIRE(f.host->run("DIMTEDIT", {ScriptItem::pick(id, {50, -30}), "80,-50"}).finished);
    auto d = f.dims();
    REQUIRE(d.size() == 1);
    CHECK(d[0]->textPosUser);
    CHECK(d[0]->textPos.x == doctest::Approx(80));
    REQUIRE(f.host->run("DIMTEDIT", {ScriptItem::pick(id, {50, -30}), "Home"}).finished);
    CHECK_FALSE(f.dims()[0]->textPosUser);
    CHECK(f.dims()[0]->textPos.x == doctest::Approx(50));
}

TEST_CASE("DIMSPACE evens out the gaps between parallel dimensions") {
    Chain f;
    REQUIRE(f.host->run("DIMLINEAR", {"0,0", "100,0", "50,-30"}).finished);
    REQUIRE(f.host->run("DIMLINEAR", {"0,0", "200,0", "50,-47"}).finished);
    REQUIRE(f.host->run("DIMLINEAR", {"0,0", "300,0", "50,-61"}).finished);
    auto dims = f.dims();
    REQUIRE(dims.size() == 3);
    std::vector<EntityId> ids;
    f.doc.forEachEntity([&](const Entity& e) { ids.push_back(e.common.id); });
    std::sort(ids.begin(), ids.end(), [](EntityId a, EntityId b) { return a.value < b.value; });
    auto r =
        f.host->run("DIMSPACE", {ScriptItem::pick(ids[0], {50, -30}),
                                 ScriptItem::select({ids[1], ids[2]}), ScriptItem::enter(), "20"});
    CHECK(r.finished);
    CHECK(entityAs<DimensionData>(*f.doc.find(ids[1]))->defPts[2].y == doctest::Approx(-50));
    CHECK(entityAs<DimensionData>(*f.doc.find(ids[2]))->defPts[2].y == doctest::Approx(-70));
    CHECK(entityAs<DimensionData>(*f.doc.find(ids[0]))->defPts[2].y == doctest::Approx(-30));
    CHECK(f.host->undo());
    CHECK(entityAs<DimensionData>(*f.doc.find(ids[1]))->defPts[2].y == doctest::Approx(-47));
}

TEST_CASE("QDIM dimensions every key point in one chain") {
    Chain f;
    auto addLine = [&](Vec2 a, Vec2 b) {
        Transaction tx = f.doc.begin("setup");
        Entity e;
        e.data = LineData{a, b};
        EntityId id = tx.add(std::move(e)).value();
        tx.commit();
        return id;
    };
    EntityId l1 = addLine({0, 0}, {40, 0});
    EntityId l2 = addLine({40, 0}, {100, 0});
    auto r = f.host->run("QDIM", {ScriptItem::select({l1, l2}), ScriptItem::enter(), "50,-20"});
    CHECK(r.finished);
    auto dims = f.dims();
    REQUIRE(dims.size() == 2);
    CHECK(dims[0]->measurement == doctest::Approx(40));
    CHECK(dims[1]->measurement == doctest::Approx(60));
    CHECK(dims[1]->defPts[2].y == doctest::Approx(-20));
    CHECK(f.host->undo());
    CHECK(f.dims().empty());
}

TEST_CASE("DIMEDIT New replaces the text and Home restores it") {
    Chain f;
    REQUIRE(f.host->run("DIMLINEAR", {"0,0", "100,0", "50,-30"}).finished);
    EntityId id{};
    f.doc.forEachEntity([&](const Entity& e) {
        if (kind(e) == EntityKind::Dimension)
            id = e.common.id;
    });
    REQUIRE(f.host->run("DIMEDIT", {"New", "TYP", ScriptItem::select({id}), ScriptItem::enter()})
                .finished);
    CHECK(f.dims()[0]->textOverride == "TYP");
    REQUIRE(
        f.host->run("DIMEDIT", {"Home", ScriptItem::select({id}), ScriptItem::enter()}).finished);
    CHECK(f.dims()[0]->textOverride.empty());
    CHECK(f.host->undo());
    CHECK(f.dims()[0]->textOverride == "TYP");
}
