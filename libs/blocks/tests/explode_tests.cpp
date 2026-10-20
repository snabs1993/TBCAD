#include "support.h"

using namespace tbcad;
using namespace tbcad::blocks;
using namespace tbcad::blocks::test;

TEST_CASE("define, insert at identity, explode returns the original geometry") {
    Document doc;
    BlockId b = addBlock(doc, "B", {line(1, 2, 3, 4), circle(5, 6, 7)}, {0, 0});
    EntityId i = addEntity(doc, insertOf(b, {0, 0}));
    auto r = explode(doc, *doc.find(i));
    REQUIRE(r.ok());
    REQUIRE(r.value().entities.size() == 2);
    const auto* ln = entityAs<LineData>(r.value().entities[0]);
    REQUIRE(ln);
    CHECK(near(ln->a, {1, 2}));
    CHECK(near(ln->b, {3, 4}));
    const auto* c = entityAs<CircleData>(r.value().entities[1]);
    REQUIRE(c);
    CHECK(near(c->center, {5, 6}));
    CHECK(c->radius == doctest::Approx(7));
}

TEST_CASE("nested inserts with rotation and scale") {
    Document doc;
    BlockId a = addBlock(doc, "A", {line(0, 0, 1, 0)});
    // B holds A twice as large, turned 90 degrees, at (10, 0).
    BlockId b = addBlock(doc, "B", {insertOf(a, {10, 0}, {2, 2}, kPi / 2)});
    EntityId i = addEntity(doc, insertOf(b, {100, 100}, {3, 3}, kPi));

    // One level: the nested insert stays an insert with the combined transform.
    auto one = explode(doc, *doc.find(i));
    REQUIRE(one.ok());
    REQUIRE(one.value().entities.size() == 1);
    const auto* ins = entityAs<InsertData>(one.value().entities[0]);
    REQUIRE(ins);
    CHECK(ins->block == a);
    CHECK(
        near(ins->position, {70, 100}));  // (10,0) scaled 3 -> (30,0), turned 180 -> (-30,0), moved
    CHECK(ins->scale.x == doctest::Approx(6));
    CHECK(normalizeAngle(ins->rotation) == doctest::Approx(normalizeAngle(kPi * 1.5)));

    // Second level gives the final line: A's (0,0)-(1,0) -> x6, rotated 270 deg.
    EntityId j = addEntity(doc, one.value().entities[0]);
    auto two = explode(doc, *doc.find(j));
    REQUIRE(two.ok());
    REQUIRE(two.value().entities.size() == 1);
    const auto* ln = entityAs<LineData>(two.value().entities[0]);
    REQUIRE(ln);
    CHECK(near(ln->a, {70, 100}));
    CHECK(near(ln->b, {70, 94}, 1e-9));

    // The same line through the snap geometry (full flatten).
    auto snap = snapGeometry(doc, *doc.find(i));
    REQUIRE(snap.size() == 1);
    CHECK(near(entityAs<LineData>(snap[0])->b, {70, 94}, 1e-9));
}

TEST_CASE("the base point maps to the insertion point") {
    Document doc;
    BlockId b = addBlock(doc, "B", {line(10, 10, 20, 10)}, {10, 10});
    EntityId i = addEntity(doc, insertOf(b, {100, 0}));
    auto r = explode(doc, *doc.find(i));
    REQUIRE(r.ok());
    const auto* ln = entityAs<LineData>(r.value().entities[0]);
    CHECK(near(ln->a, {100, 0}));
    CHECK(near(ln->b, {110, 0}));
}

TEST_CASE("exploding resolves layer 0 and ByBlock from the insert") {
    Document doc;
    LayerId walls;
    {
        Transaction tx = doc.begin("layer");
        walls = tx.addLayer(Layer{.name = "Walls", .color = Color::index(3)}).value();
        tx.commit();
    }
    Entity l0 = line(0, 0, 1, 0);
    l0.common.layer = kLayerZero;
    l0.common.color = Color::byBlock();
    l0.common.lineweight = LineWeight::byBlock();
    Entity l1 = line(0, 1, 1, 1);
    l1.common.layer = kLayerZero;
    l1.common.color = Color::index(5);  // explicit colour is kept
    BlockId b = addBlock(doc, "B", {l0, l1});
    Entity ins = insertOf(b, {0, 0});
    ins.common.layer = walls;
    ins.common.color = Color::index(2);
    ins.common.lineweight = LineWeight::hundredths(50);
    EntityId i = addEntity(doc, ins);
    auto r = explode(doc, *doc.find(i));
    REQUIRE(r.ok());
    REQUIRE(r.value().entities.size() == 2);
    const Entity& e0 = r.value().entities[0];
    CHECK(e0.common.layer == walls);
    CHECK(e0.common.color == Color::index(2));
    CHECK(e0.common.lineweight == LineWeight::hundredths(50));
    const Entity& e1 = r.value().entities[1];
    CHECK(e1.common.layer == walls);
    CHECK(e1.common.color == Color::index(5));
}

TEST_CASE("a circle in a non-uniformly scaled insert explodes to an ellipse") {
    Document doc;
    BlockId b = addBlock(doc, "B", {circle(0, 0, 1)});
    EntityId i = addEntity(doc, insertOf(b, {5, 5}, {4, 2}));
    auto r = explode(doc, *doc.find(i));
    REQUIRE(r.ok());
    REQUIRE(r.value().entities.size() == 1);
    const auto* el = entityAs<EllipseData>(r.value().entities[0]);
    REQUIRE(el);
    CHECK(near(el->center, {5, 5}));
    CHECK(length(el->majorAxis) == doctest::Approx(4));
    CHECK(el->ratio == doctest::Approx(0.5));
}

TEST_CASE("a polyline explodes to lines and arcs - widths are reported") {
    Entity pl;
    pl.data = PolylineData{{{{0, 0}, 0, 1, 1}, {{10, 0}, 1, 0, 0}, {{10, 10}, 0, 0, 0}}, false, 0};
    auto r = explode(Document{}, pl);
    REQUIRE(r.ok());
    REQUIRE(r.value().entities.size() == 2);
    CHECK(entityAs<LineData>(r.value().entities[0]));
    const auto* arc = entityAs<ArcData>(r.value().entities[1]);
    REQUIRE(arc);
    CHECK(arc->radius == doctest::Approx(5));
    CHECK_FALSE(r.value().messages.empty());
    // Arc endpoints match the polyline vertices (a bulge of 1 is a half circle, counter-clockwise).
    Vec2 s{arc->center.x + arc->radius * std::cos(arc->startAngle),
           arc->center.y + arc->radius * std::sin(arc->startAngle)};
    Vec2 e{arc->center.x + arc->radius * std::cos(arc->endAngle),
           arc->center.y + arc->radius * std::sin(arc->endAngle)};
    CHECK(near(s, {10, 0}, 1e-9));
    CHECK(near(e, {10, 10}, 1e-9));
}

TEST_CASE("a closed polyline gives its closing segment - negative bulge runs clockwise") {
    Entity pl;
    pl.data = PolylineData{{{{0, 0}, -1, 0, 0}, {{10, 0}, 0, 0, 0}, {{10, 10}, 0, 0, 0}}, true, 0};
    auto r = explode(Document{}, pl);
    REQUIRE(r.ok());
    REQUIRE(r.value().entities.size() == 3);
    const auto* arc = entityAs<ArcData>(r.value().entities[0]);
    REQUIRE(arc);
    // Clockwise from (0,0) to (10,0): the arc, drawn counter-clockwise, goes from (10,0) to (0,0).
    Vec2 s{arc->center.x + arc->radius * std::cos(arc->startAngle),
           arc->center.y + arc->radius * std::sin(arc->startAngle)};
    CHECK(near(s, {10, 0}, 1e-9));
}

TEST_CASE("exploding a polyline with arcs under non-uniform scale splits it") {
    Document doc;
    Entity pl;
    pl.data = PolylineData{{{{0, 0}, 1, 0, 0}, {{2, 0}, 0, 0, 0}}, false, 0};
    BlockId b = addBlock(doc, "B", {pl});
    EntityId i = addEntity(doc, insertOf(b, {0, 0}, {3, 1}));
    auto r = explode(doc, *doc.find(i));
    REQUIRE(r.ok());
    REQUIRE(r.value().entities.size() == 1);
    CHECK(entityAs<EllipseData>(r.value().entities[0]));
    CHECK_FALSE(r.value().messages.empty());
}

TEST_CASE("text in a non-uniformly scaled insert is scaled evenly with a message") {
    Document doc;
    Entity t;
    t.data = Indirect<TextData>(TextData{.position = {1, 0}, .height = 2, .utf8 = "A"});
    BlockId b = addBlock(doc, "B", {t});
    EntityId i = addEntity(doc, insertOf(b, {0, 0}, {4, 1}));
    auto r = explode(doc, *doc.find(i));
    REQUIRE(r.ok());
    REQUIRE(r.value().entities.size() == 1);
    const auto* td = entityAs<TextData>(r.value().entities[0]);
    REQUIRE(td);
    CHECK(near(td->position, {4, 0}));
    CHECK(td->height == doctest::Approx(4));  // sqrt(4*1) = 2 times 2
    CHECK_FALSE(r.value().messages.empty());
}

TEST_CASE("kinds without a hook cannot be exploded - hooks plug in") {
    Entity pt;
    pt.data = PointData{{1, 1}};
    CHECK_FALSE(explode(Document{}, pt).ok());
    CHECK_FALSE(canExplode(EntityKind::Point));
    ExplodeRegistry::set(EntityKind::Point,
                         [](const Document&, const Entity& e) -> Result<ExplodeResult> {
                             ExplodeResult r;
                             r.entities.push_back(e);
                             return r;
                         });
    CHECK(canExplode(EntityKind::Point));
    auto r = explode(Document{}, pt);
    CHECK(r.ok());
    ExplodeRegistry::clear(EntityKind::Point);
    CHECK_FALSE(canExplode(EntityKind::Point));
}

TEST_CASE("mirrored and rotated inserts put every point where the transform sends it") {
    Document doc;
    BlockId b = addBlock(doc, "M", {line(1, 0, 3, 0), circle(2, 1, 0.5)});
    {  // mirrored in x
        EntityId i = addEntity(doc, insertOf(b, {10, 10}, {-1, 1}));
        auto r = explode(doc, *doc.find(i));
        REQUIRE(r.ok());
        REQUIRE(r.value().entities.size() == 2);
        const auto* ln = entityAs<LineData>(r.value().entities[0]);
        REQUIRE(ln);
        CHECK(near(ln->a, {9, 10}));
        CHECK(near(ln->b, {7, 10}));
        const auto* c = entityAs<CircleData>(r.value().entities[1]);
        REQUIRE(c);
        CHECK(near(c->center, {8, 11}));
        CHECK(c->radius == doctest::Approx(0.5));
    }
    {  // mirrored in y and turned 90 degrees
        EntityId i = addEntity(doc, insertOf(b, {10, 10}, {1, -1}, kPi / 2));
        auto r = explode(doc, *doc.find(i));
        REQUIRE(r.ok());
        const auto* ln = entityAs<LineData>(r.value().entities[0]);
        REQUIRE(ln);
        CHECK(near(ln->a, {10, 11}, 1e-9));
        CHECK(near(ln->b, {10, 13}, 1e-9));
        const auto* c = entityAs<CircleData>(r.value().entities[1]);
        REQUIRE(c);
        CHECK(near(c->center, {11, 12}, 1e-9));
    }
}

TEST_CASE("an arc in a mirrored insert keeps its middle point and stays counter-clockwise") {
    Document doc;
    Entity arc;
    arc.data = ArcData{{0, 0}, 2, 0, kPi / 2};
    BlockId b = addBlock(doc, "A", {arc});
    const double h = std::sqrt(2.0);
    for (Vec2 s : {Vec2{-1, 1}, Vec2{1, -1}, Vec2{-2, 2}}) {
        CAPTURE(s.x);
        CAPTURE(s.y);
        EntityId i = addEntity(doc, insertOf(b, {10, 0}, s));
        auto r = explode(doc, *doc.find(i));
        REQUIRE(r.ok());
        REQUIRE(r.value().entities.size() == 1);
        const auto* a = entityAs<ArcData>(r.value().entities[0]);
        REQUIRE(a);
        double sweep = std::fmod(a->endAngle - a->startAngle, 2 * kPi);
        if (sweep < 0)
            sweep += 2 * kPi;
        CHECK(sweep == doctest::Approx(kPi / 2));
        const double mid = a->startAngle + sweep / 2;
        const Vec2 got = a->center + Vec2{std::cos(mid), std::sin(mid)} * a->radius;
        CHECK(near(got, {10 + s.x * h, s.y * h}, 1e-9));
        CHECK(a->radius == doctest::Approx(2 * std::abs(s.x)));
    }
}

TEST_CASE("attributes of nested inserts take the value of their own insert") {
    Document doc;
    TextData td;
    td.position = {0, 1};
    td.utf8 = "TAG";
    td.attdef = AttDefInfo{"TAG", "", "default", false, false, false, false};
    Entity def;
    def.data = Indirect<TextData>(std::move(td));
    BlockId inner = addBlock(doc, "Inner", {line(0, 0, 1, 0), def});
    BlockId outer = addBlock(doc, "Outer", {insertOf(inner, {5, 0}), insertOf(inner, {0, 5})});
    // The inner inserts inside the block keep their own values when the outer one is exploded
    // twice.
    EntityId o = addEntity(doc, insertOf(outer, {100, 0}, {-1, 1}));
    auto one = explode(doc, *doc.find(o));
    REQUIRE(one.ok());
    REQUIRE(one.value().entities.size() == 2);
    for (const Entity& e : one.value().entities) {
        const auto* ins = entityAs<InsertData>(e);
        REQUIRE(ins);
        CHECK(ins->block == inner);
    }
    CHECK(near(entityAs<InsertData>(one.value().entities[0])->position, {95, 0}));
    CHECK(near(entityAs<InsertData>(one.value().entities[1])->position, {100, 5}));
    // (-1, 1) and (1, -1) turned half a circle are the same mirror: look at what comes out.
    const Entity ins0 = one.value().entities[0];
    EntityId i0 = addEntity(doc, ins0);
    auto two = explode(doc, *doc.find(i0));
    REQUIRE(two.ok());
    const auto* ln = entityAs<LineData>(two.value().entities[0]);
    REQUIRE(ln);
    CHECK(near(ln->a, {95, 0}));
    CHECK(near(ln->b, {94, 0}));
    const auto* tx = entityAs<TextData>(two.value().entities[1]);
    REQUIRE(tx);
    CHECK(tx->utf8 == "default");
    CHECK(near(tx->position, {95, 1}));
}

TEST_CASE("a dimension and a multileader in a scaled, mirrored insert follow the transform") {
    Document doc;
    DimensionData d;
    d.kind = DimKind::Linear;
    d.defPts = {Vec2{0, 0}, Vec2{4, 0}, Vec2{4, 2}, Vec2{}};
    d.textPos = {2, 2};
    d.measurement = 4;
    Entity dim;
    dim.data = Indirect<DimensionData>(d);
    MLeaderData ml;
    ml.lines = {{{0, 0}, {1, 1}}};
    ml.landing = {1, 1};
    ml.contentPos = {2, 1};
    ml.utf8 = "note";
    Entity lead;
    lead.data = Indirect<MLeaderData>(ml);
    BlockId b = addBlock(doc, "D", {dim, lead});
    EntityId i = addEntity(doc, insertOf(b, {10, 0}, {-2, 2}));
    auto r = explode(doc, *doc.find(i));
    REQUIRE(r.ok());
    REQUIRE(r.value().entities.size() == 2);
    const auto* dd = entityAs<DimensionData>(r.value().entities[0]);
    REQUIRE(dd);
    CHECK(near(dd->defPts[0], {10, 0}));
    CHECK(near(dd->defPts[1], {2, 0}));
    CHECK(near(dd->textPos, {6, 4}));
    CHECK(dd->measurement == doctest::Approx(8));
    const auto* mm = entityAs<MLeaderData>(r.value().entities[1]);
    REQUIRE(mm);
    REQUIRE(mm->lines.size() == 1);
    CHECK(near(mm->lines[0][0], {10, 0}));
    CHECK(near(mm->lines[0][1], {8, 2}));
    CHECK(near(mm->landing, {8, 2}));
    CHECK(near(mm->contentPos, {6, 2}));
}
