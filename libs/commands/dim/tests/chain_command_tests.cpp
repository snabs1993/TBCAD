#include <doctest/doctest.h>

#include <cmath>

#include "tbcad/cmd/cmd.h"
#include "tbcad/cmd/script_host.h"
#include "tbcad/dim/dim.h"
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

TEST_CASE("DIMEDIT Rotate turns the text and Oblique leans the extension lines") {
    Chain f;
    REQUIRE(f.host->run("DIMLINEAR", {"0,0", "100,0", "50,-30"}).finished);
    EntityId id{};
    f.doc.forEachEntity([&](const Entity& e) {
        if (kind(e) == EntityKind::Dimension)
            id = e.common.id;
    });
    auto layout = [&] {
        return dim::layoutDimension(f.doc, *entityAs<DimensionData>(*f.doc.find(id)));
    };
    REQUIRE(f.host->run("DIMEDIT", {"Rotate", "30", ScriptItem::select({id}), ScriptItem::enter()})
                .finished);
    CHECK(layout().text.rotation == doctest::Approx(30.0 * 3.14159265358979 / 180));
    REQUIRE(f.host->run("DIMEDIT", {"Oblique", "60", ScriptItem::select({id}), ScriptItem::enter()})
                .finished);
    int ext = 0;
    for (const auto& l : layout().lines) {
        if (l.role != dim::Role::ExtLine)
            continue;
        ++ext;
        const Vec2 d = normalized(l.pts[1] - l.pts[0]);
        CHECK(std::abs(d.x * std::cos(1.0471975512) + d.y * std::sin(1.0471975512)) ==
              doctest::Approx(1.0));
        CHECK(l.pts[1].y < -30);  // runs past the dimension line, as extension lines do
    }
    CHECK(ext == 2);
    // Saved with the dimension.
    CHECK(entityAs<DimensionData>(*f.doc.find(id))->oblique != 0);
    REQUIRE(
        f.host->run("DIMEDIT", {"Home", ScriptItem::select({id}), ScriptItem::enter()}).finished);
    CHECK_FALSE(entityAs<DimensionData>(*f.doc.find(id))->textRotated);
    REQUIRE(f.host
                ->run("DIMEDIT", {"Oblique", ScriptItem::enter(), ScriptItem::select({id}),
                                  ScriptItem::enter()})
                .finished);
    CHECK(entityAs<DimensionData>(*f.doc.find(id))->oblique == 0);
}

TEST_CASE("DIMBREAK opens a gap in the dimension line where another object crosses it") {
    Chain f;
    REQUIRE(f.host->run("DIMLINEAR", {"0,0", "100,0", "50,-30"}).finished);
    {
        auto tx = f.doc.begin("cutter");
        Entity cut;
        cut.data = LineData{{40, -50}, {40, 10}};  // crosses the dimension line (y = -30) at x = 40
        REQUIRE(tx.add(std::move(cut)).ok());
        tx.commit();
    }
    EntityId id{};
    f.doc.forEachEntity([&](const Entity& e) {
        if (kind(e) == EntityKind::Dimension)
            id = e.common.id;
    });
    auto count = [&] {
        const Entity* e = f.doc.find(id);
        const dim::Layout L = dim::layoutDimension(f.doc, *entityAs<DimensionData>(*e));
        std::size_t dimLines = 0;
        for (const auto& l : L.lines)
            if (l.role == dim::Role::DimLine)
                ++dimLines;
        return dimLines;
    };
    const std::size_t before = count();
    REQUIRE(f.host->run("DIMBREAK", {ScriptItem::select({id}), ScriptItem::enter(), "Auto", "All"})
                .finished);
    const auto* d = entityAs<DimensionData>(*f.doc.find(id));
    REQUIRE(d->breaks.size() == 1);
    CHECK(d->breaks[0].a.x < 40);
    CHECK(d->breaks[0].b.x > 40);
    CHECK(count() == before + 1);  // the line is now in two pieces

    // The gaps are saved with the dimension.
    CHECK(f.host->undo());
    CHECK(entityAs<DimensionData>(*f.doc.find(id))->breaks.empty());
    REQUIRE(f.host->run("DIMBREAK", {ScriptItem::select({id}), ScriptItem::enter(), "Auto", "All"})
                .finished);
    REQUIRE(f.host->run("DIMBREAK", {ScriptItem::select({id}), ScriptItem::enter(), "Remove"})
                .finished);
    CHECK(entityAs<DimensionData>(*f.doc.find(id))->breaks.empty());
    CHECK(count() == before);
}
