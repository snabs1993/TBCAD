#include <doctest/doctest.h>

#include <cmath>

#include "tbcad/cmd/cmd.h"
#include "tbcad/cmd/script_host.h"
#include "tbcad/core/dim_assoc.h"
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
    f.host->run("DIMREASSOC", {ScriptItem::select({dimId}), ScriptItem::enter()});
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
    f.host->run("DIMREASSOC", {ScriptItem::select({dimId}), ScriptItem::enter()});
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
