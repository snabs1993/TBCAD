#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>

#include "tbcad/cmd/cmd.h"
#include "tbcad/cmd/script_host.h"
#include "tbcad/core/dim_assoc.h"
#include "tbcad/dim/dim.h"
#include "tbcad/dimcmd/dimcmd.h"
#include "tbcad/text/text.h"

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

TEST_CASE("DIMCONTINUE and DIMBASELINE dimensions take the current layer and colour") {
    Chain f;
    {
        Transaction tx = f.doc.begin("current");
        Layer l;
        l.name = "DIMS";
        const LayerId dims = tx.addLayer(l).value();
        REQUIRE(tx.modifyHeader([&](Header& h) {
                      h.currentLayer = dims;
                      h.currentColor = Color::index(3);
                  })
                    .ok());
        tx.commit();
    }
    REQUIRE(f.host->run("DIMLINEAR", {"0,0", "100,0", "50,-30"}).finished);
    REQUIRE(f.host->run("DIMCONTINUE", {"250,0", ScriptItem::enter()}).finished);
    REQUIRE(f.host->run("DIMBASELINE", {"400,0", ScriptItem::enter()}).finished);
    int n = 0;
    f.doc.forEachEntity([&](const Entity& e) {
        if (!entityAs<DimensionData>(e))
            return;
        ++n;
        const Layer* l = f.doc.layer(e.common.layer);
        REQUIRE(l);
        CHECK(l->name == "DIMS");
        CHECK(e.common.color.kind == ColorKind::Index);
        CHECK(e.common.color.indexValue() == 3);
    });
    CHECK(n == 3);
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

TEST_CASE("DIMCONTINUE Undo takes back the last dimension and chains from the one before") {
    Chain f;
    REQUIRE(f.host->run("DIMLINEAR", {"0,0", "100,0", "50,-30"}).finished);
    auto r = f.host->run("DIMCONTINUE", {"250,0", "400,0", "Undo", "300,0", ScriptItem::enter()});
    CHECK(r.finished);
    auto dims = f.dims();
    REQUIRE(dims.size() == 3);
    CHECK(dims[2]->defPts[0].x == doctest::Approx(250));
    CHECK(dims[2]->defPts[1].x == doctest::Approx(300));
    CHECK(f.host->undo());  // the whole run is one undo step
    CHECK(f.dims().size() == 1);
}

TEST_CASE("DIMCONTINUE goes on around an angular dimension at the same radius") {
    Chain f;
    REQUIRE(f.host->run("DIMANGULAR", {"", "0,0", "50,0", "0,50", "30,30"}).finished);
    // from the 90 degree side on to 135 degrees
    REQUIRE(f.host->run("DIMCONTINUE", {"-30,30", ScriptItem::enter()}).finished);
    auto dims = f.dims();
    REQUIRE(dims.size() == 2);
    CHECK(dims[1]->kind == DimKind::Angular3Point);
    CHECK(dims[1]->measurement == doctest::Approx(kPi / 4));
    CHECK(dim::angularGeometry(*dims[1]).radius ==
          doctest::Approx(dim::angularGeometry(*dims[0]).radius));
}

TEST_CASE("DIMBASELINE on an angular dimension measures from its first side, further out") {
    Chain f;
    REQUIRE(f.host->run("DIMANGULAR", {"", "0,0", "50,0", "0,50", "30,30"}).finished);
    REQUIRE(f.host->run("DIMBASELINE", {"-30,30", ScriptItem::enter()}).finished);
    auto dims = f.dims();
    REQUIRE(dims.size() == 2);
    CHECK(dims[1]->measurement == doctest::Approx(3 * kPi / 4));
    CHECK(dim::angularGeometry(*dims[1]).radius > dim::angularGeometry(*dims[0]).radius + 1e-6);
}

TEST_CASE("DIMCONTINUE on an ordinate dimension keeps the axis and lines up the leader ends") {
    Chain f;
    REQUIRE(f.host->run("DIMORDINATE", {"120,45", "120,80"}).finished);
    REQUIRE(f.host->run("DIMCONTINUE", {"200,30", "260,10", ScriptItem::enter()}).finished);
    auto dims = f.dims();
    REQUIRE(dims.size() == 3);
    CHECK(dims[1]->kind == DimKind::Ordinate);
    CHECK(dims[1]->measurement == doctest::Approx(200));
    CHECK(dims[1]->defPts[1].y == doctest::Approx(dims[0]->defPts[1].y));
    CHECK(dims[2]->measurement == doctest::Approx(260));
    CHECK(dims[2]->defPts[1].x == doctest::Approx(260));
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
    // DIMTMOVE 0: the dimension line follows the text, which sits above it, as in AutoCAD.
    CHECK(d[0]->defPts[2].y == doctest::Approx(-50 - 1.875));
    CHECK(dim::layoutDimension(f.doc, *d[0]).text.center.y == doctest::Approx(-50));
    REQUIRE(f.host->run("DIMTEDIT", {ScriptItem::pick(id, {50, -30}), "Home"}).finished);
    CHECK_FALSE(f.dims()[0]->textPosUser);
    CHECK(f.dims()[0]->textPos.x == doctest::Approx(50));
}

TEST_CASE("DIMFLIPARROW toggles one arrowhead and undoes") {
    Chain f;
    REQUIRE(f.host->run("DIMLINEAR", {"0,0", "100,0", "50,-30"}).finished);
    EntityId id{};
    f.doc.forEachEntity([&](const Entity& e) {
        if (kind(e) == EntityKind::Dimension)
            id = e.common.id;
    });
    REQUIRE(f.host->run("DIMFLIPARROW", {ScriptItem::pick(id, {50, -30}), "Second"}).finished);
    CHECK_FALSE(f.dims()[0]->flipArrow1);
    CHECK(f.dims()[0]->flipArrow2);
    REQUIRE(f.host->run("DIMFLIPARROW", {ScriptItem::pick(id, {50, -30}), ""}).finished);
    CHECK(f.dims()[0]->flipArrow1);
    REQUIRE(f.host->run("DIMFLIPARROW", {ScriptItem::pick(id, {50, -30}), "Second"}).finished);
    CHECK_FALSE(f.dims()[0]->flipArrow2);
    f.host->undo();
    CHECK(f.dims()[0]->flipArrow2);
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

// DIMSPACE on angular dimensions spaces the arcs about their shared vertex, as in AutoCAD.
TEST_CASE("DIMSPACE spaces angular dimensions about their vertex") {
    Chain f;
    const double c = std::sqrt(0.5);
    auto at = [&](double r) { return std::to_string(r * c) + "," + std::to_string(r * c); };
    REQUIRE(f.host->run("DIMANGULAR", {"", "0,0", "50,0", "0,50", at(10)}).finished);
    REQUIRE(f.host->run("DIMANGULAR", {"", "0,0", "50,0", "0,50", at(13)}).finished);
    REQUIRE(f.host->run("DIMANGULAR", {"", "0,0", "50,0", "0,50", at(30)}).finished);
    REQUIRE(f.host->run("DIMLINEAR", {"0,0", "100,0", "50,-30"}).finished);
    std::vector<EntityId> ids;
    f.doc.forEachEntity([&](const Entity& e) { ids.push_back(e.common.id); });
    std::sort(ids.begin(), ids.end(), [](EntityId a, EntityId b) { return a.value < b.value; });
    REQUIRE(ids.size() == 4);
    auto radius = [&](EntityId id) {
        return dim::angularGeometry(*entityAs<DimensionData>(*f.doc.find(id))).radius;
    };
    REQUIRE(radius(ids[0]) == doctest::Approx(10));
    auto r = f.host->run(
        "DIMSPACE", {ScriptItem::pick(ids[0], {7, 7}), ScriptItem::select({ids[1], ids[2], ids[3]}),
                     ScriptItem::enter(), "5"});
    CHECK(r.finished);
    CHECK(radius(ids[0]) == doctest::Approx(10));
    CHECK(radius(ids[1]) == doctest::Approx(15));
    CHECK(radius(ids[2]) == doctest::Approx(20));
    CHECK(entityAs<DimensionData>(*f.doc.find(ids[3]))->defPts[2].y == doctest::Approx(-30));
    CHECK(f.host->undo());
    CHECK(radius(ids[1]) == doctest::Approx(13));
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

TEST_CASE("QDIM on existing dimensions replaces them with the new set") {
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
    REQUIRE(f.host->run("QDIM", {ScriptItem::select({l1, l2}), ScriptItem::enter(), "50,-20"})
                .finished);
    std::vector<EntityId> old;
    f.doc.forEachEntity([&](const Entity& e) {
        if (kind(e) == EntityKind::Dimension)
            old.push_back(e.common.id);
    });
    REQUIRE(old.size() == 2);
    // The two dimensions and one more line, as a baseline set.
    EntityId l3 = addLine({100, 0}, {130, 0});
    std::vector<EntityId> picked = old;
    picked.push_back(l3);
    REQUIRE(
        f.host->run("QDIM", {ScriptItem::select(picked), ScriptItem::enter(), "Baseline", "50,-30"})
            .finished);
    for (EntityId id : old)
        CHECK(f.doc.find(id) == nullptr);
    auto dims = f.dims();
    REQUIRE(dims.size() == 3);
    std::vector<double> m;
    for (const auto* d : dims)
        m.push_back(d->measurement);
    std::sort(m.begin(), m.end());
    CHECK(m[0] == doctest::Approx(40));
    CHECK(m[1] == doctest::Approx(100));
    CHECK(m[2] == doctest::Approx(130));
    CHECK(f.host->undo());
    CHECK(f.dims().size() == 2);
}

// QDIM Edit removes key points by picking them and Adds new ones; seTtings Intersection also
// dimensions where the selected objects cross, as in AutoCAD.
TEST_CASE("QDIM Edit removes and adds points, seTtings Intersection adds the crossings") {
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
    auto r = f.host->run("QDIM", {ScriptItem::select({l1, l2}), ScriptItem::enter(), "Edit",
                                  "40.5,0.5", "Add", "70,0", "eXit", "50,-20"});
    CHECK(r.finished);
    auto dims = f.dims();
    REQUIRE(dims.size() == 2);
    CHECK(dims[0]->measurement == doctest::Approx(70));
    CHECK(dims[1]->measurement == doctest::Approx(30));

    Chain g;
    auto addLine2 = [&](Vec2 a, Vec2 b) {
        Transaction tx = g.doc.begin("setup");
        Entity e;
        e.data = LineData{a, b};
        EntityId id = tx.add(std::move(e)).value();
        tx.commit();
        return id;
    };
    EntityId a = addLine2({0, 0}, {100, 0});
    EntityId b = addLine2({30, -10}, {60, 10});
    r = g.host->run("QDIM", {ScriptItem::select({a, b}), ScriptItem::enter(), "seTtings",
                             "Intersection", "50,-30"});
    CHECK(r.finished);
    dims = g.dims();
    REQUIRE(dims.size() == 4);  // 0, 30, 45 (the crossing), 60, 100
    CHECK(dims[1]->measurement == doctest::Approx(15));
}

TEST_CASE("QDIM Baseline measures from the first point and steps each line outwards") {
    Chain f;
    Transaction tx = f.doc.begin("setup");
    Entity e;
    e.data = PolylineData{{{{0, 0}}, {{40, 0}}, {{100, 0}}, {{150, 0}}}, false};
    EntityId id = tx.add(std::move(e)).value();
    tx.commit();
    auto r =
        f.host->run("QDIM", {ScriptItem::select({id}), ScriptItem::enter(), "Baseline", "50,-20"});
    CHECK(r.finished);
    auto dims = f.dims();
    REQUIRE(dims.size() == 3);
    std::vector<double> m, y;
    for (const DimensionData* d : dims) {
        m.push_back(d->measurement);
        y.push_back(d->defPts[2].y);
    }
    std::sort(m.begin(), m.end());
    CHECK(m[0] == doctest::Approx(40));
    CHECK(m[1] == doctest::Approx(100));
    CHECK(m[2] == doctest::Approx(150));
    std::sort(y.begin(), y.end());
    CHECK(y[0] < y[1]);  // stepped outwards (downwards here)
    CHECK(y[2] == doctest::Approx(-20));
}

namespace {
EntityId fourPointPolyline(Chain& f) {
    Transaction tx = f.doc.begin("setup");
    Entity e;
    e.data = PolylineData{{{{0, 0}}, {{40, 0}}, {{100, 0}}, {{150, 0}}}, false};
    EntityId id = tx.add(std::move(e)).value();
    tx.commit();
    return id;
}
std::vector<double> sortedMeasurements(const Chain& f) {
    std::vector<double> m;
    for (const DimensionData* d : f.dims())
        m.push_back(d->measurement);
    std::sort(m.begin(), m.end());
    return m;
}
}  // namespace

TEST_CASE("QDIM Staggered nests the pairs from the outside in") {
    Chain f;
    EntityId id = fourPointPolyline(f);
    REQUIRE(
        f.host->run("QDIM", {ScriptItem::select({id}), ScriptItem::enter(), "Staggered", "50,-20"})
            .finished);
    const auto m = sortedMeasurements(f);
    REQUIRE(m.size() == 2);
    CHECK(m[0] == doctest::Approx(60));   // 40..100, at the picked line
    CHECK(m[1] == doctest::Approx(150));  // 0..150, one step further out
    for (const DimensionData* d : f.dims()) {
        if (d->measurement < 100)
            CHECK(d->defPts[2].y == doctest::Approx(-20));
        else
            CHECK(d->defPts[2].y < -20 - 1);
    }
}

TEST_CASE("QDIM Ordinate measures every key point from the datum point") {
    Chain f;
    EntityId id = fourPointPolyline(f);
    REQUIRE(f.host
                ->run("QDIM", {ScriptItem::select({id}), ScriptItem::enter(), "datumPoint", "40,0",
                               "Ordinate", "50,-20"})
                .finished);
    auto dims = f.dims();
    REQUIRE(dims.size() == 4);
    for (const DimensionData* d : dims) {
        CHECK(d->kind == DimKind::Ordinate);
        CHECK(d->defPts[1].y == doctest::Approx(-20));
    }
    const auto m = sortedMeasurements(f);
    CHECK(m[0] == doctest::Approx(0));  // the datum itself
    CHECK(m[3] == doctest::Approx(110));
}

TEST_CASE("QDIM Baseline with a datum point measures from it") {
    Chain f;
    EntityId id = fourPointPolyline(f);
    REQUIRE(f.host
                ->run("QDIM", {ScriptItem::select({id}), ScriptItem::enter(), "Baseline",
                               "datumPoint", "100,0", "50,-20"})
                .finished);
    const auto m = sortedMeasurements(f);
    REQUIRE(m.size() == 3);
    CHECK(m[0] == doctest::Approx(50));
    CHECK(m[1] == doctest::Approx(60));
    CHECK(m[2] == doctest::Approx(100));
}

TEST_CASE("QDIM Radius dimensions every circle of the selection") {
    Chain f;
    Transaction tx = f.doc.begin("setup");
    Entity a;
    a.data = CircleData{{0, 0}, 5};
    Entity b;
    b.data = CircleData{{30, 0}, 8};
    EntityId ia = tx.add(std::move(a)).value();
    EntityId ib = tx.add(std::move(b)).value();
    tx.commit();
    f.host->run("QDIM", {ScriptItem::select({ia, ib}), ScriptItem::enter(), "Radius"});
    auto dims = f.dims();
    REQUIRE(dims.size() == 2);
    CHECK(dims[0]->kind == DimKind::Radius);
    CHECK(dims[0]->measurement + dims[1]->measurement == doctest::Approx(13));
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
    // Home puts the text back in its place and angle; the replaced text stays, as in AutoCAD.
    CHECK(f.dims()[0]->textOverride == "TYP");
    REQUIRE(f.host->run("DIMEDIT", {"New", "<>", ScriptItem::select({id}), ScriptItem::enter()})
                .finished);
    CHECK(f.dims()[0]->textOverride.empty());
    CHECK(f.host->undo());
    CHECK(f.dims()[0]->textOverride == "TYP");
}

TEST_CASE("DIMTEDIT Left, Right and Center line the text up along the dimension line") {
    Chain f;
    REQUIRE(f.host->run("DIMLINEAR", {"0,0", "100,0", "50,-30"}).finished);
    EntityId id{};
    f.doc.forEachEntity([&](const Entity& e) {
        if (kind(e) == EntityKind::Dimension)
            id = e.common.id;
    });
    auto dimNow = [&] { return entityAs<DimensionData>(*f.doc.find(id)); };
    auto layout = [&] { return dim::layoutDimension(f.doc, *dimNow()); };
    REQUIRE(f.host->run("DIMTEDIT", {ScriptItem::pick(id, {50, -30}), "Left"}).finished);
    CHECK(dimNow()->textPosUser);
    CHECK(dimNow()->textPos.y == doctest::Approx(-30));
    const double w = layout().text.width;
    // Two arrow lengths and a gap from the first extension line to the edge of the text.
    CHECK(layout().text.center.x == doctest::Approx(2 * 2.5 + 0.625 + w / 2));
    REQUIRE(f.host->run("DIMTEDIT", {ScriptItem::pick(id, {50, -30}), "Right"}).finished);
    CHECK(layout().text.center.x == doctest::Approx(100 - (2 * 2.5 + 0.625 + w / 2)));
    REQUIRE(f.host->run("DIMTEDIT", {ScriptItem::pick(id, {50, -30}), "Center"}).finished);
    CHECK(layout().text.center.x == doctest::Approx(50));
    // Angle turns the text; 0 puts it back along the line.
    REQUIRE(f.host->run("DIMTEDIT", {ScriptItem::pick(id, {50, -30}), "Angle", "45"}).finished);
    CHECK(dimNow()->textRotated);
    CHECK(layout().text.rotation == doctest::Approx(kPi / 4));
    REQUIRE(f.host->run("DIMTEDIT", {ScriptItem::pick(id, {50, -30}), "Angle", "0"}).finished);
    CHECK_FALSE(dimNow()->textRotated);
    // Left on a vertical dimension is the bottom end (the text reads bottom to top).
    REQUIRE(f.host->run("DIMLINEAR", {"200,0", "200,100", "230,50"}).finished);
    EntityId v{};
    f.doc.forEachEntity([&](const Entity& e) {
        if (kind(e) == EntityKind::Dimension && e.common.id != id)
            v = e.common.id;
    });
    REQUIRE(f.host->run("DIMTEDIT", {ScriptItem::pick(v, {230, 50}), "Left"}).finished);
    CHECK(entityAs<DimensionData>(*f.doc.find(v))->textPos.y < 50);
    CHECK(f.host->undo());
    CHECK_FALSE(entityAs<DimensionData>(*f.doc.find(v))->textPosUser);
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

// As AutoCAD: the arc of an angular dimension breaks too, and other dimensions and text break a
// dimension where they cross it.
TEST_CASE("DIMBREAK breaks angular arcs, and at other dimensions and text") {
    text::registerDisplay();
    Chain f;
    auto add = [&](EntityData data) {
        auto tx = f.doc.begin("setup");
        Entity e;
        e.data = std::move(data);
        EntityId id = tx.add(std::move(e)).value();
        tx.commit();
        return id;
    };
    auto newest = [&] {
        EntityId id{};
        f.doc.forEachEntity([&](const Entity& e) {
            if (kind(e) == EntityKind::Dimension && e.common.id.value > id.value)
                id = e.common.id;
        });
        return id;
    };
    auto dimOf = [&](EntityId id) { return entityAs<DimensionData>(*f.doc.find(id)); };

    // An angular dimension (arc radius 30) crossed by a line at 45 degrees.
    REQUIRE(f.host->run("DIMANGULAR", {"", "0,0", "50,0", "0,50", "21.2132,21.2132"}).finished);
    const EntityId ang = newest();
    const std::size_t arcsBefore = dim::layoutDimension(f.doc, *dimOf(ang)).arcs.size();
    add(LineData{{10, 10}, {40, 40}});
    REQUIRE(f.host->run("DIMBREAK", {ScriptItem::select({ang}), ScriptItem::enter(), "Auto", "All"})
                .finished);
    REQUIRE(dimOf(ang)->breaks.size() == 1);
    CHECK(distance(dimOf(ang)->breaks[0].a, {0, 0}) == doctest::Approx(30));
    CHECK(dim::layoutDimension(f.doc, *dimOf(ang)).arcs.size() == arcsBefore + 1);

    // A horizontal dimension crossed by the dimension line of a vertical one at x = 140, and by a
    // text at x = 170. The vertical one's extension lines end where they start: no break there.
    REQUIRE(f.host->run("DIMLINEAR", {"100,0", "200,0", "150,-30"}).finished);
    const EntityId h = newest();
    REQUIRE(f.host->run("DIMLINEAR", {"160,-60", "160,20", "140,-20"}).finished);
    const EntityId v = newest();
    REQUIRE(dimOf(v)->defPts[2].x == doctest::Approx(140));
    TextData t;
    t.position = {168, -31};
    t.height = 2.5;
    t.utf8 = "XX";
    add(t);
    REQUIRE(f.host->run("DIMBREAK", {ScriptItem::select({h}), ScriptItem::enter(), "Auto", "All"})
                .finished);
    const auto& br = dimOf(h)->breaks;
    REQUIRE(br.size() == 2);
    std::vector<double> mids;
    for (const DimBreak& b : br) {
        CHECK(b.a.y == doctest::Approx(-30));
        mids.push_back((b.a.x + b.b.x) / 2);
    }
    std::sort(mids.begin(), mids.end());
    CHECK(mids[0] == doctest::Approx(140));
    CHECK(mids[1] > 168);
    CHECK(mids[1] < 173);
    // The gap round the text takes the whole text box, not one gap at each side of it.
    const DimBreak& textGap = br[0].a.x > 150 ? br[0] : br[1];
    CHECK(std::min(textGap.a.x, textGap.b.x) < 168);
    CHECK(std::max(textGap.a.x, textGap.b.x) > 170);
}

// As AutoCAD: automatic gaps follow the dimension and the objects that break it; gaps placed by
// hand stay where they are.
TEST_CASE("DIMBREAK Auto gaps follow the objects and the dimension, Manual gaps stay") {
    Chain f;
    REQUIRE(f.host->run("DIMLINEAR", {"0,0", "100,0", "50,-30"}).finished);
    EntityId dimId{}, cutter{};
    f.doc.forEachEntity([&](const Entity& e) { dimId = e.common.id; });
    {
        auto tx = f.doc.begin("cutter");
        Entity cut;
        cut.data = LineData{{40, -50}, {40, 10}};
        cutter = tx.add(std::move(cut)).value();
        tx.commit();
    }
    auto dim = [&] { return entityAs<DimensionData>(*f.doc.find(dimId)); };
    auto gapAt = [&](std::size_t i) {
        const DimBreak& b = dim()->breaks.at(i);
        return Vec2{(b.a.x + b.b.x) / 2, (b.a.y + b.b.y) / 2};
    };
    auto edit = [&](EntityId id, auto fn) {
        auto tx = f.doc.begin("edit");
        REQUIRE(tx.modify(id, fn));
        tx.commit();
    };
    REQUIRE(f.host
                ->run("DIMBREAK", {ScriptItem::select({dimId}), ScriptItem::enter(), "Auto",
                                   ScriptItem::enter()})
                .finished);
    REQUIRE(dim()->breaks.size() == 1);
    CHECK(dim()->autoBreaks == 1);
    REQUIRE(dim()->breakWith.size() == 1);
    CHECK(dim()->breakWith[0] == cutter);

    // The line moves: its gap goes with it.
    edit(cutter, [](Entity& e) { e.data = LineData{{60, -50}, {60, 10}}; });
    REQUIRE(dim()->breaks.size() == 1);
    CHECK(gapAt(0).x == doctest::Approx(60));

    // A gap by hand, then the line moves again: only the automatic gap follows.
    REQUIRE(f.host
                ->run("DIMBREAK", {ScriptItem::select({dimId}), ScriptItem::enter(), "Manual",
                                   "80,-30", "85,-30"})
                .finished);
    edit(cutter, [](Entity& e) { e.data = LineData{{20, -50}, {20, 10}}; });
    REQUIRE(dim()->breaks.size() == 2);
    CHECK(gapAt(0).x == doctest::Approx(20));
    CHECK(gapAt(1).x == doctest::Approx(82.5));

    // The dimension line moves: the automatic gap is found again on it.
    edit(dimId, [](Entity& e) { entityAs<DimensionData>(e)->defPts[2].y = -40; });
    CHECK(gapAt(0).y == doctest::Approx(-40));
    CHECK(gapAt(0).x == doctest::Approx(20));
    REQUIRE(dim()->breaks.size() == 2);
    CHECK(gapAt(1).y == doctest::Approx(-30));  // the gap by hand stays where it was put

    // The line is erased: its gap goes; the one by hand stays.
    {
        auto tx = f.doc.begin("erase");
        REQUIRE(tx.erase(cutter));
        tx.commit();
    }
    REQUIRE(dim()->breaks.size() == 1);
    CHECK(gapAt(0).x == doctest::Approx(82.5));
    CHECK(dim()->breakWith.empty());
}

TEST_CASE("DIMBREAK Manual opens a gap between two picked points on one line") {
    Chain f;
    REQUIRE(f.host->run("DIMLINEAR", {"0,0", "100,0", "50,-30"}).finished);
    EntityId id{};
    f.doc.forEachEntity([&](const Entity& e) {
        if (kind(e) == EntityKind::Dimension)
            id = e.common.id;
    });
    // the points snap onto the dimension line (y = -30)
    REQUIRE(f.host
                ->run("DIMBREAK",
                      {ScriptItem::select({id}), ScriptItem::enter(), "Manual", "20,-29", "35,-31"})
                .finished);
    const auto* d = entityAs<DimensionData>(*f.doc.find(id));
    REQUIRE(d->breaks.size() == 1);
    CHECK(d->breaks[0].a.x == doctest::Approx(20));
    CHECK(d->breaks[0].a.y == doctest::Approx(-30));
    CHECK(d->breaks[0].b.x == doctest::Approx(35));
    // a second manual break adds to the first
    REQUIRE(f.host
                ->run("DIMBREAK",
                      {ScriptItem::select({id}), ScriptItem::enter(), "Manual", "60,-30", "70,-30"})
                .finished);
    CHECK(entityAs<DimensionData>(*f.doc.find(id))->breaks.size() == 2);
    CHECK(f.host->undo());
    CHECK(entityAs<DimensionData>(*f.doc.find(id))->breaks.size() == 1);
}

TEST_CASE("DIMJOGLINE puts a Z-shaped jog in the dimension line and Remove takes it out") {
    Chain f;
    REQUIRE(f.host->run("DIMLINEAR", {"0,0", "100,0", "50,-30"}).finished);
    EntityId id{};
    f.doc.forEachEntity([&](const Entity& e) {
        if (kind(e) == EntityKind::Dimension)
            id = e.common.id;
    });
    auto shape = [&] {
        const dim::Layout L =
            dim::layoutDimension(f.doc, *entityAs<DimensionData>(*f.doc.find(id)));
        std::size_t zig = 0, straight = 0;
        for (const auto& l : L.lines) {
            if (l.role != dim::Role::DimLine)
                continue;
            (l.pts.size() == 4 ? zig : straight)++;
        }
        return std::pair{zig, straight};
    };
    const auto before = shape();
    REQUIRE(f.host->run("DIMJOGLINE", {ScriptItem::pick(id, {10, -30}), "20,-30"}).finished);
    const auto* d = entityAs<DimensionData>(*f.doc.find(id));
    REQUIRE(d->jogs.size() == 1);
    CHECK(d->jogs[0].x == doctest::Approx(20));
    CHECK(d->jogs[0].y == doctest::Approx(-30));
    const auto after = shape();
    CHECK(after.first == 1);
    CHECK(after.second == before.second + 1);  // the dimension line is cut in two around the Z
    CHECK(f.host->undo());
    CHECK(entityAs<DimensionData>(*f.doc.find(id))->jogs.empty());

    // Enter = the middle of the dimension line; Remove clears every jog.
    REQUIRE(
        f.host->run("DIMJOGLINE", {ScriptItem::pick(id, {10, -30}), ScriptItem::enter()}).finished);
    CHECK(entityAs<DimensionData>(*f.doc.find(id))->jogs.size() == 1);
    REQUIRE(f.host->run("DIMJOGLINE", {ScriptItem::pick(id, {10, -30}), "Remove"}).finished);
    CHECK(entityAs<DimensionData>(*f.doc.find(id))->jogs.empty());
    CHECK(shape() == before);

    // It survives the file format and a transform.
    REQUIRE(f.host->run("DIMJOGLINE", {ScriptItem::pick(id, {10, -30}), "20,-30"}).finished);
    Entity moved = *f.doc.find(id);
    auto t = geom::transformed(moved, Transform2::translate({5, 5}));
    REQUIRE(t.ok());
    CHECK(entityAs<DimensionData>(t.value())->jogs[0].x == doctest::Approx(25));
}

// As AutoCAD: one jog per dimension (a new one replaces the old), Remove at the first prompt
// takes the jog of the dimension picked next, and the jog stays on the dimension line when the
// line is moved.
TEST_CASE("DIMJOGLINE: one jog per dimension, Remove first, the jog follows the dimension line") {
    Chain f;
    REQUIRE(f.host->run("DIMLINEAR", {"0,0", "100,0", "50,-30"}).finished);
    REQUIRE(f.host->run("DIMLINEAR", {"0,0", "100,0", "50,-60"}).finished);
    std::vector<EntityId> ids;
    f.doc.forEachEntity([&](const Entity& e) { ids.push_back(e.common.id); });
    std::sort(ids.begin(), ids.end(), [](EntityId a, EntityId b) { return a.value < b.value; });
    REQUIRE(ids.size() == 2);
    auto zigzags = [&](EntityId id) {
        const dim::Layout L =
            dim::layoutDimension(f.doc, *entityAs<DimensionData>(*f.doc.find(id)));
        std::size_t n = 0;
        for (const auto& l : L.lines)
            if (l.role == dim::Role::DimLine && l.pts.size() == 4)
                ++n;
        return n;
    };
    REQUIRE(f.host->run("DIMJOGLINE", {ScriptItem::pick(ids[0], {10, -30}), "20,-30"}).finished);
    REQUIRE(f.host->run("DIMJOGLINE", {ScriptItem::pick(ids[0], {10, -30}), "80,-30"}).finished);
    const auto* d = entityAs<DimensionData>(*f.doc.find(ids[0]));
    REQUIRE(d->jogs.size() == 1);
    CHECK(d->jogs[0].x == doctest::Approx(80));
    CHECK(zigzags(ids[0]) == 1);

    // Moving the dimension line (DIMSPACE here) keeps the jog on it.
    REQUIRE(f.host
                ->run("DIMSPACE", {ScriptItem::pick(ids[1], {50, -60}),
                                   ScriptItem::select({ids[0]}), ScriptItem::enter(), "10"})
                .finished);
    CHECK(entityAs<DimensionData>(*f.doc.find(ids[0]))->defPts[2].y == doctest::Approx(-50));
    CHECK(zigzags(ids[0]) == 1);

    REQUIRE(f.host->run("DIMJOGLINE", {ScriptItem::pick(ids[1], {10, -60}), "30,-60"}).finished);
    REQUIRE(f.host->run("DIMJOGLINE", {"Remove", ScriptItem::pick(ids[0], {10, -50})}).finished);
    CHECK(entityAs<DimensionData>(*f.doc.find(ids[0]))->jogs.empty());
    CHECK(entityAs<DimensionData>(*f.doc.find(ids[1]))->jogs.size() == 1);
    CHECK(zigzags(ids[0]) == 0);
}

TEST_CASE("an associative dimension follows its line, a dimension moved alone loses the link") {
    Chain f;
    EntityId line;
    {
        auto tx = f.doc.begin("line");
        Entity e;
        e.data = LineData{{0, 0}, {100, 0}};
        line = tx.add(std::move(e)).value();
        tx.commit();
    }
    REQUIRE(f.host->run("DIMLINEAR", {"0,0", "100,0", "50,-30"}).finished);
    REQUIRE(f.dims().size() == 1);
    CHECK(f.dims()[0]->assoc[0].kind == DimAssoc::Kind::Vertex);
    CHECK(f.dims()[0]->assoc[0].target == line);
    CHECK(f.dims()[0]->assoc[1].index == 1);

    // Stretch the end of the line: the dimension point and the measurement follow.
    {
        auto tx = f.doc.begin("stretch");
        REQUIRE(tx.modify(line, [](Entity& e) { entityAs<LineData>(e)->b = {150, 20}; }));
        tx.commit();
    }
    const DimensionData* d = f.dims()[0];
    CHECK(d->defPts[1].x == doctest::Approx(150));
    CHECK(d->defPts[1].y == doctest::Approx(20));
    CHECK(d->measurement == doctest::Approx(150));
    {  // the transactions of this test are not on the host's undo stack: put the end back by hand
        auto tx = f.doc.begin("stretch back");
        REQUIRE(tx.modify(line, [](Entity& e) { entityAs<LineData>(e)->b = {100, 0}; }));
        tx.commit();
    }
    CHECK(f.dims()[0]->defPts[1].x == doctest::Approx(100));

    // Moving the dimension alone cuts the link; the line then no longer drags it.
    EntityId dimId;
    f.doc.forEachEntity([&](const Entity& e) {
        if (entityAs<DimensionData>(e))
            dimId = e.common.id;
    });
    {
        auto tx = f.doc.begin("move dim");
        REQUIRE(tx.modify(dimId, [](Entity& e) {
            auto* dm = entityAs<DimensionData>(e);
            for (Vec2& p : dm->defPts)
                p += Vec2{0, 500};
        }));
        tx.commit();
    }
    CHECK(f.dims()[0]->assoc[0].kind == DimAssoc::Kind::None);
    {
        auto tx = f.doc.begin("stretch again");
        REQUIRE(tx.modify(line, [](Entity& e) { entityAs<LineData>(e)->b = {300, 0}; }));
        tx.commit();
    }
    CHECK(f.dims()[0]->defPts[1].x == doctest::Approx(100));

    // DIMREASSOC links by position again only where a point sits on the line's ends.
    f.host->run("DIMREASSOC", {ScriptItem::select({dimId}), ScriptItem::enter(),
                               ScriptItem::enter(), ScriptItem::enter()});
    CHECK(f.dims()[0]->assoc[0].kind == DimAssoc::Kind::None);
}

TEST_CASE("DIMDISASSOC cuts the links of a dimension") {
    Chain f;
    {
        auto tx = f.doc.begin("line");
        Entity e;
        e.data = LineData{{0, 0}, {100, 0}};
        (void)tx.add(std::move(e));
        tx.commit();
    }
    REQUIRE(f.host->run("DIMLINEAR", {"0,0", "100,0", "50,-30"}).finished);
    EntityId dimId;
    f.doc.forEachEntity([&](const Entity& e) {
        if (entityAs<DimensionData>(e))
            dimId = e.common.id;
    });
    f.host->run("DIMDISASSOC", {ScriptItem::select({dimId}), ScriptItem::enter()});
    CHECK(f.dims()[0]->assoc[0].kind == DimAssoc::Kind::None);
    f.host->run("DIMREASSOC", {ScriptItem::select({dimId}), ScriptItem::enter(),
                               ScriptItem::enter(), ScriptItem::enter()});
    CHECK(f.dims()[0]->assoc[0].kind == DimAssoc::Kind::Vertex);
}

TEST_CASE("moving the measured line moves the dimension line with it, stretching does not") {
    Chain f;
    EntityId line;
    {
        auto tx = f.doc.begin("line");
        Entity e;
        e.data = LineData{{0, 0}, {6000, 0}};
        line = tx.add(std::move(e)).value();
        tx.commit();
    }
    REQUIRE(f.host->run("DIMLINEAR", {"0,0", "6000,0", "3000,-800"}).finished);
    REQUIRE(f.dims().size() == 1);

    // The whole line moves 500 up: the dimension line keeps its 800 distance.
    {
        auto tx = f.doc.begin("move");
        REQUIRE(tx.modify(line, [](Entity& e) {
            auto* l = entityAs<LineData>(e);
            l->a = {0, 500};
            l->b = {6000, 500};
        }));
        tx.commit();
    }
    const DimensionData* d = f.dims()[0];
    CHECK(d->defPts[0].y == doctest::Approx(500));
    CHECK(d->defPts[1].y == doctest::Approx(500));
    CHECK(d->defPts[2].x == doctest::Approx(3000));
    CHECK(d->defPts[2].y == doctest::Approx(-300));
    CHECK(d->measurement == doctest::Approx(6000));

    // One end moves: the dimension line stays where it is.
    {
        auto tx = f.doc.begin("stretch");
        REQUIRE(tx.modify(line, [](Entity& e) { entityAs<LineData>(e)->b = {7000, 700}; }));
        tx.commit();
    }
    CHECK(f.dims()[0]->defPts[2].y == doctest::Approx(-300));
    CHECK(f.dims()[0]->defPts[1].x == doctest::Approx(7000));
}

TEST_CASE(
    "DIMINSPECT adds and removes an inspection mark; DIMREGEN keeps linked dimensions right") {
    Chain f;
    {
        auto tx = f.doc.begin("line");
        Entity e;
        e.data = LineData{{0, 0}, {100, 0}};
        (void)tx.add(std::move(e));
        tx.commit();
    }
    REQUIRE(f.host->run("DIMLINEAR", {"0,0", "100,0", "50,-30"}).finished);
    EntityId dimId;
    f.doc.forEachEntity([&](const Entity& e) {
        if (entityAs<DimensionData>(e))
            dimId = e.common.id;
    });
    f.host->setSelection({dimId});
    REQUIRE(f.host->run("DIMINSPECT", {"Add", "QC", "50"}).finished);
    CHECK(f.dims()[0]->textOverride == "<> [QC 50%]");
    f.host->setSelection({dimId});
    f.host->run("DIMINSPECT", {"Add", "", "100"});  // replaces the earlier mark
    CHECK(f.dims()[0]->textOverride == "<> [100%]");
    f.host->setSelection({dimId});
    f.host->run("DIMINSPECT", {"Remove"});
    CHECK(f.dims()[0]->textOverride.empty());

    const double before = f.dims()[0]->measurement;
    REQUIRE(f.host->run("DIMREGEN", {}).finished);
    CHECK(f.host->logContains("1 dimensions updated"));
    CHECK(f.dims()[0]->measurement == doctest::Approx(before));
    CHECK(f.host->undo());
}

TEST_CASE("DIMUPDATE gives dimensions the current style") {
    Chain f;
    REQUIRE(f.host->run("DIMLINEAR", {"0,0", "100,0", "50,-30"}).finished);
    EntityId dimId;
    f.doc.forEachEntity([&](const Entity& e) {
        if (entityAs<DimensionData>(e))
            dimId = e.common.id;
    });
    f.host->run("DIMSTYLE", {"New", "Alt", "Done"});
    DimStyleId alt{};
    for (const auto& [id, st] : f.doc.dimStyles())
        if (st.name == "Alt")
            alt = id;
    REQUIRE(alt);
    {
        Transaction tx = f.doc.begin("style");
        REQUIRE(tx.modify(dimId, [&](Entity& e) { entityAs<DimensionData>(e)->style = alt; }));
        tx.commit();
    }
    REQUIRE(f.dims()[0]->style == alt);
    f.host->setSelection({dimId});
    REQUIRE(f.host->run("DIMUPDATE", {}).finished);
    CHECK(f.dims()[0]->style == f.doc.header().currentDimStyle);
}

TEST_CASE("DIMREASSOCIATE asks for each point: a pick links it there, Select object takes a line") {
    Chain f;
    EntityId line{}, circle{};
    {
        auto tx = f.doc.begin("objects");
        Entity l;
        l.data = LineData{{0, 50}, {80, 50}};
        line = tx.add(std::move(l)).value();
        Entity c;
        c.data = CircleData{{200, 0}, 10};
        circle = tx.add(std::move(c)).value();
        tx.commit();
    }
    // A dimension measuring nothing, then linked point by point to the line's ends.
    REQUIRE(f.host->run("DIMLINEAR", {"0,0", "100,0", "50,-30"}).finished);
    EntityId dimId{};
    f.doc.forEachEntity([&](const Entity& e) {
        if (entityAs<DimensionData>(e))
            dimId = e.common.id;
    });
    REQUIRE(f.host
                ->run("DIMREASSOCIATE",
                      {ScriptItem::select({dimId}), ScriptItem::enter(), "0,50", "80,50"})
                .finished);
    auto dimNow = [&] { return *entityAs<DimensionData>(*f.doc.find(dimId)); };
    CHECK(dimNow().assoc[0].target == line);
    CHECK(dimNow().assoc[1].target == line);
    CHECK(dimNow().measurement == doctest::Approx(80));
    // A pick on nothing keeps the point as it is.
    REQUIRE(f.host
                ->run("DIMREASSOCIATE", {ScriptItem::select({dimId}), ScriptItem::enter(),
                                         "500,500", ScriptItem::enter()})
                .finished);
    CHECK(dimNow().measurement == doctest::Approx(80));
    // Select object: the ends of the line, the nearer one first.
    REQUIRE(
        f.host->run("DIMDISASSOC", {ScriptItem::select({dimId}), ScriptItem::enter()}).finished);
    REQUIRE(f.host
                ->run("DIMREASSOCIATE", {ScriptItem::select({dimId}), ScriptItem::enter(), "Select",
                                         ScriptItem::pick(line, {40, 50})})
                .finished);
    CHECK(dimNow().assoc[0].index == 0);
    CHECK(dimNow().assoc[1].index == 1);
    // A radius dimension linked to a circle by Select object follows it.
    DimensionData r;
    r.kind = DimKind::Radius;
    r.defPts = {Vec2{0, 0}, Vec2{5, 5}, Vec2{}, Vec2{}};
    r.textPos = {10, 10};
    EntityId rid{};
    {
        auto tx = f.doc.begin("radius");
        Entity e;
        e.data = Indirect<DimensionData>(r);
        rid = tx.add(std::move(e)).value();
        tx.commit();
    }
    REQUIRE(f.host
                ->run("DIMREASSOCIATE", {ScriptItem::select({rid}), ScriptItem::enter(), "Select",
                                         ScriptItem::pick(circle, {210, 0})})
                .finished);
    const auto rd = *entityAs<DimensionData>(*f.doc.find(rid));
    CHECK(rd.assoc[0].kind == DimAssoc::Kind::Center);
    CHECK(rd.assoc[1].kind == DimAssoc::Kind::OnCurve);
    CHECK(rd.measurement == doctest::Approx(10));
    {
        auto tx = f.doc.begin("grow");
        REQUIRE(tx.modify(circle, [](Entity& e) { entityAs<CircleData>(e)->radius = 25; }));
        tx.commit();
    }
    CHECK(entityAs<DimensionData>(*f.doc.find(rid))->measurement == doctest::Approx(25));
}

TEST_CASE("DIMBREAK breaks leaders and multileaders, and the gaps follow") {
    Chain f;
    EntityId leader{}, mleader{}, cutter{};
    {
        auto tx = f.doc.begin("objects");
        Entity l;
        l.data = LeaderData{{{0, 0}, {50, 0}, {60, 10}}, {}, "", {}};
        leader = tx.add(std::move(l)).value();
        MLeaderData md;
        md.lines = {{{0, 20}, {50, 20}}};
        md.landing = {50, 20};
        md.contentPos = {60, 20};
        md.dogleg = true;
        Entity m;
        m.data = md;
        mleader = tx.add(std::move(m)).value();
        Entity cut;
        cut.data = LineData{{30, -10}, {30, 30}};
        cutter = tx.add(std::move(cut)).value();
        tx.commit();
    }
    REQUIRE(f.host
                ->run("DIMBREAK", {ScriptItem::select({leader, mleader}), ScriptItem::enter(),
                                   "Auto", ScriptItem::enter()})
                .finished);
    const auto* ld = entityAs<LeaderData>(*f.doc.find(leader));
    const auto* md = entityAs<MLeaderData>(*f.doc.find(mleader));
    REQUIRE(ld->breaks.size() == 1);
    REQUIRE(md->breaks.size() == 1);
    CHECK(ld->autoBreaks == 1);
    CHECK(md->breakWith == std::vector<EntityId>{cutter});
    CHECK((ld->breaks[0].a.x + ld->breaks[0].b.x) / 2 == doctest::Approx(30));
    // The leader line is drawn in two pieces round the gap.
    const dim::Layout L = dim::layoutLeader(f.doc, *ld);
    std::size_t pieces = 0;
    for (const auto& line : L.lines)
        if (!line.pts.empty() && line.pts.front().y == doctest::Approx(0) &&
            line.pts.back().y == doctest::Approx(0))
            ++pieces;
    CHECK(pieces == 1);  // the piece from the tip to the gap; the other runs on to (60,10)
    CHECK(dim::layoutMLeader(f.doc, *md).lines.size() >= 3);

    // The line moves: both gaps go with it.
    {
        auto tx = f.doc.begin("move");
        REQUIRE(tx.modify(cutter, [](Entity& e) { e.data = LineData{{10, -10}, {10, 30}}; }));
        tx.commit();
    }
    ld = entityAs<LeaderData>(*f.doc.find(leader));
    md = entityAs<MLeaderData>(*f.doc.find(mleader));
    REQUIRE(ld->breaks.size() == 1);
    REQUIRE(md->breaks.size() == 1);
    CHECK((ld->breaks[0].a.x + ld->breaks[0].b.x) / 2 == doctest::Approx(10));
    CHECK((md->breaks[0].a.x + md->breaks[0].b.x) / 2 == doctest::Approx(10));

    REQUIRE(f.host
                ->run("DIMBREAK",
                      {ScriptItem::select({leader, mleader}), ScriptItem::enter(), "Remove"})
                .finished);
    CHECK(entityAs<LeaderData>(*f.doc.find(leader))->breaks.empty());
    CHECK(entityAs<MLeaderData>(*f.doc.find(mleader))->breakWith.empty());
}

TEST_CASE("DIMANGULAR Quadrant puts the arc on the side of the picked point") {
    Chain f;
    // the cursor is inside the 90 degree sector, the quadrant point is in the reflex one
    REQUIRE(f.host->run("DIMANGULAR", {"", "0,0", "50,0", "0,50", "Quadrant", "-10,-10", "30,30"})
                .finished);
    auto dims = f.dims();
    REQUIRE(dims.size() == 1);
    CHECK(dims[0]->measurement == doctest::Approx(3 * kPi / 2));
}
