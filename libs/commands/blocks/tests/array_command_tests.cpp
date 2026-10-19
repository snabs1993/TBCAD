#include "support.h"

using namespace tbcad;
using namespace tbcad::block_cmds;
using namespace tbcad::block_cmds::test;

TEST_CASE("ARRAY rectangular creates the grid of copies") {
    Fixture f;
    EntityId c = f.addCircle(0, 0, 1);
    f.host->setSelection({c});
    // rows 2, columns 3, column spacing 5, row spacing 10, plain copies
    auto r =
        f.host->run("ARRAY", {"R", "Rows", "2", "Columns", "3", "Spacing", "5", "10", "As", ""});
    CHECK(r.finished);
    REQUIRE(f.modelSpace().size() == 6);
    std::vector<Vec2> centers;
    for (EntityId id : f.modelSpace())
        centers.push_back(entityAs<CircleData>(*f.doc.find(id))->center);
    auto has = [&](Vec2 p) {
        for (Vec2 q : centers)
            if (near(p, q))
                return true;
        return false;
    };
    CHECK(has({0, 0}));
    CHECK(has({5, 0}));
    CHECK(has({10, 0}));
    CHECK(has({0, 10}));
    CHECK(has({10, 10}));
    CHECK(f.host->undoStack().undoLabel() == "Array 5 objects");
    CHECK(f.host->undo());
    CHECK(f.modelSpace().size() == 1);
}

TEST_CASE("ARRAY polar of 6 around a centre") {
    Fixture f;
    EntityId c = f.addCircle(10, 0, 1);
    f.host->setSelection({c});
    auto r = f.host->run("ARRAY", {"PO", "0,0", "As", ""});
    CHECK(r.finished);
    REQUIRE(f.modelSpace().size() == 6);
    for (int k = 0; k < 6; ++k) {
        double a = kTwoPi * k / 6;
        bool found = false;
        for (EntityId id : f.modelSpace())
            if (near(entityAs<CircleData>(*f.doc.find(id))->center,
                     {10 * std::cos(a), 10 * std::sin(a)}, 1e-9))
                found = true;
        CHECK(found);
    }
}

TEST_CASE("ARRAY accepts the defaults with Enter") {
    Fixture f;
    EntityId c = f.addCircle(0, 0, 1);
    f.host->setSelection({c});
    f.host->run("ARRAY", {"", "As", ""});
    CHECK(f.modelSpace().size() == 12);  // 3 rows x 4 columns
}

TEST_CASE("ARRAY refuses a huge array and Esc changes nothing") {
    Fixture f;
    EntityId c = f.addCircle(0, 0, 1);
    f.host->setSelection({c});
    f.host->run("ARRAY", {"R", "Rows", "5000", "Columns", "5000", ""});
    CHECK(f.modelSpace().size() == 1);
    f.host->setSelection({c});
    f.host->run("ARRAY", {"R", "Rows", "2", cmd::ScriptItem::esc()});
    CHECK(f.modelSpace().size() == 1);
}

TEST_CASE("ARRAY previews the copies while the last distance is chosen") {
    Fixture f;
    EntityId c = f.addCircle(0, 0, 1);
    f.host->setSelection({c});
    f.host->run("ARRAY", {"R", "Rows", "2", "Spacing", "5"});
    f.host->runner().pointerMoved(cmd::RawInput{{0, 20}, {0, 20}, 1});
    CHECK_FALSE(f.host->preview().items.empty());
}

namespace {

const InsertData* onlyInsert(Fixture& f) {
    for (EntityId id : f.modelSpace())
        if (const auto* ins = entityAs<InsertData>(*f.doc.find(id)))
            return ins;
    return nullptr;
}

}  // namespace

TEST_CASE("ARRAY is associative by default: one insert of an anonymous block") {
    Fixture f;
    EntityId c = f.addCircle(0, 0, 1);
    f.host->setSelection({c});
    auto r = f.host->run("ARRAY", {"R", "Rows", "2", "Columns", "3", "Spacing", "5", "10", ""});
    CHECK(r.finished);
    REQUIRE(f.modelSpace().size() == 1);
    const InsertData* ins = onlyInsert(f);
    REQUIRE(ins);
    REQUIRE(ins->array);
    CHECK(ins->array->kind == ArrayInfo::Kind::Rectangular);
    CHECK(ins->array->rows == 2);
    CHECK(ins->array->columns == 3);
    const BlockDef* def = f.doc.block(ins->block);
    REQUIRE(def);
    CHECK(def->anonymous);
    CHECK(def->name[0] == '*');
    CHECK(def->entities.size() == 1);
    // Six circles are drawn and the bounds cover the whole grid.
    BBox2 box = blocks::insertBounds(f.doc, *f.doc.find(f.modelSpace()[0]));
    CHECK(near(box.min, {-1, -1}));
    CHECK(near(box.max, {11, 11}));
    auto ex = blocks::explode(f.doc, *f.doc.find(f.modelSpace()[0]));
    REQUIRE(ex.ok());
    CHECK(ex.value().entities.size() == 6);
    CHECK(f.host->undo());
    CHECK(f.modelSpace().size() == 1);
    CHECK(entityAs<CircleData>(*f.doc.find(f.modelSpace()[0])));
}

TEST_CASE("ARRAYPOLAR makes an associative ring that follows the insert") {
    Fixture f;
    EntityId c = f.addCircle(10, 0, 1);
    f.host->setSelection({c});
    f.host->run("ARRAYPOLAR", {"0,0", "Items", "4", ""});
    const InsertData* ins = onlyInsert(f);
    REQUIRE(ins);
    REQUIRE(ins->array);
    CHECK(ins->array->kind == ArrayInfo::Kind::Polar);
    CHECK(ins->array->count == 4);
    auto ex = blocks::explode(f.doc, *f.doc.find(f.modelSpace()[0]));
    REQUIRE(ex.ok());
    REQUIRE(ex.value().entities.size() == 4);
    CHECK(near(entityAs<CircleData>(ex.value().entities[1])->center, {0, 10}, 1e-9));
    CHECK(near(entityAs<CircleData>(ex.value().entities[2])->center, {-10, 0}, 1e-9));
}

TEST_CASE("ARRAYPATH spaces the items along a line") {
    Fixture f;
    EntityId c = f.addCircle(0, 0, 1);
    EntityId path = f.addLine(0, 0, 20, 0);
    f.host->setSelection({c});
    // path, base point (Enter = centre), count, 5 items, align
    f.host->run("ARRAYPATH", {cmd::ScriptItem::pick(path), "", "", "5", "N"});
    const InsertData* ins = onlyInsert(f);
    REQUIRE(ins);
    REQUIRE(ins->array);
    CHECK(ins->array->kind == ArrayInfo::Kind::Path);
    EntityId arrayId;
    for (EntityId id : f.modelSpace())
        if (entityAs<InsertData>(*f.doc.find(id)))
            arrayId = id;
    auto ex = blocks::explode(f.doc, *f.doc.find(arrayId));
    REQUIRE(ex.ok());
    REQUIRE(ex.value().entities.size() == 5);
    for (int k = 0; k < 5; ++k)
        CHECK(near(entityAs<CircleData>(ex.value().entities[static_cast<std::size_t>(k)])->center,
                   {5.0 * k, 0}, 1e-9));
}

TEST_CASE("ARRAYEDIT changes the rows and spacing of an associative array") {
    Fixture f;
    EntityId c = f.addCircle(0, 0, 1);
    f.host->setSelection({c});
    f.host->run("ARRAY", {"R", "Rows", "2", "Columns", "2", "Spacing", "5", "5", ""});
    EntityId arr = f.modelSpace()[0];
    f.host->run("ARRAYEDIT", {cmd::ScriptItem::pick(arr), "Rows", "4", "7", "X"});
    const InsertData* ins = onlyInsert(f);
    REQUIRE(ins);
    REQUIRE(ins->array);
    CHECK(ins->array->rows == 4);
    CHECK(ins->array->columns == 2);
    CHECK(ins->array->rowSpacing == doctest::Approx(7));
    CHECK(ins->array->columnSpacing == doctest::Approx(5));
    CHECK(f.host->undo());
    CHECK(onlyInsert(f)->array->rows == 2);
}

TEST_CASE("ARRAY shows the grid while the row and column counts are typed") {
    Fixture f;
    EntityId c = f.addCircle(0, 0, 1);
    f.host->setSelection({c});
    auto r = f.host->run("ARRAY", {"R", "Rows"});  // now at "number of rows"
    CHECK(!r.finished);
    f.host->runner().textEdited("2");
    CHECK(!f.host->preview().items.empty());
    const std::size_t two = f.host->preview().items.size();
    f.host->runner().textEdited("4");
    CHECK(f.host->preview().items.size() > two);
    f.host->runner().textEdited("");
    CHECK(f.host->preview().items.empty());
}

TEST_CASE("ARRAY with every default is one Enter and the spacing has no long decimals") {
    Fixture f;
    EntityId c = f.addCircle(0, 0, 7.3);  // extent 14.6, spacing 1.5 x = 21.9
    f.host->setSelection({c});
    auto r = f.host->run("ARRAY", {"", ""});
    CHECK(r.finished);
    REQUIRE(f.modelSpace().size() == 1);
    const InsertData* ins = onlyInsert(f);
    REQUIRE(ins);
    REQUIRE(ins->array);
    CHECK(ins->array->rows == 3);
    CHECK(ins->array->columns == 4);
    CHECK(ins->array->columnSpacing == doctest::Approx(22));
    CHECK(ins->array->rowSpacing == doctest::Approx(22));
}

TEST_CASE("ARRAYEDIT changes count, fill angle and rotation of a polar array, with undo") {
    Fixture f;
    EntityId c = f.addCircle(10, 0, 1);
    f.host->setSelection({c});
    f.host->run("ARRAYPOLAR", {"0,0", "Items", "4", ""});
    EntityId arr = f.modelSpace()[0];
    f.host->run("ARRAYEDIT",
                {cmd::ScriptItem::pick(arr), "Items", "6", "F", "180", "ROT", "No", ""});
    const InsertData* ins = onlyInsert(f);
    REQUIRE(ins);
    REQUIRE(ins->array);
    CHECK(ins->array->count == 6);
    CHECK(ins->array->fillAngle == doctest::Approx(kPi));
    CHECK(!ins->array->rotateItems);
    CHECK(f.host->undo());
    CHECK(onlyInsert(f)->array->count == 4);
    CHECK(onlyInsert(f)->array->rotateItems);
}

TEST_CASE("ARRAYEDIT changes the count and alignment of a path array") {
    Fixture f;
    EntityId c = f.addCircle(0, 0, 1);
    EntityId path = f.addLine(0, 0, 20, 0);
    f.host->setSelection({c});
    f.host->run("ARRAYPATH", {cmd::ScriptItem::pick(path), "", "", "5", "N"});
    EntityId arr;
    for (EntityId id : f.modelSpace())
        if (entityAs<InsertData>(*f.doc.find(id)))
            arr = id;
    f.host->run("ARRAYEDIT", {cmd::ScriptItem::pick(arr), "I", "3", "Align", "Yes", ""});
    const InsertData* ins = nullptr;
    f.doc.forEachEntity([&](const Entity& e) {
        if (const auto* i = entityAs<InsertData>(e))
            ins = i;
    });
    REQUIRE(ins);
    REQUIRE(ins->array);
    CHECK(ins->array->count == 3);
    CHECK(ins->array->rotateItems);
}

TEST_CASE("ARRAYPATH follows an ellipse and a spline, items on the curve") {
    Fixture f;
    EntityId c = f.addCircle(10, 0, 0.5);
    EllipseData el;
    el.center = {0, 0};
    el.majorAxis = {10, 0};
    el.ratio = 0.5;
    Entity ee;
    ee.data = Indirect<EllipseData>(el);
    EntityId path = f.add(ee);
    f.host->setSelection({c});
    // path, base point = the circle's centre, count, 4 items, not aligned
    f.host->run("ARRAYPATH", {cmd::ScriptItem::pick(path), "10,0", "", "4", "N"});
    EntityId arr;
    for (EntityId id : f.modelSpace())
        if (entityAs<InsertData>(*f.doc.find(id)))
            arr = id;
    REQUIRE(arr);
    REQUIRE(entityAs<InsertData>(*f.doc.find(arr))->array->pathClosed);
    auto ex = blocks::explode(f.doc, *f.doc.find(arr));
    REQUIRE(ex.ok());
    REQUIRE(ex.value().entities.size() == 4);
    for (const Entity& e : ex.value().entities) {
        const Vec2 p = entityAs<CircleData>(e)->center;
        CHECK((p.x / 10) * (p.x / 10) + (p.y / 5) * (p.y / 5) == doctest::Approx(1).epsilon(1e-3));
    }
    // Closed: the four items are spread round the whole ellipse, not doubled at the seam.
    CHECK(near(entityAs<CircleData>(ex.value().entities[0])->center, {10, 0}, 1e-6));
    CHECK(entityAs<CircleData>(ex.value().entities[2])->center.x < -9.9);

    SplineData sp;
    sp.degree = 3;
    sp.knots = {0, 0, 0, 0, 1, 1, 1, 1};
    sp.control = {{0, 20}, {10, 30}, {20, 10}, {30, 20}};
    Entity se;
    se.data = Indirect<SplineData>(sp);
    EntityId sPath = f.add(se);
    EntityId d = f.addCircle(0, 20, 0.5);
    f.host->setSelection({d});
    f.host->run("ARRAYPATH", {cmd::ScriptItem::pick(sPath), "0,20", "", "5", "Y"});
    EntityId arr2;
    for (EntityId id : f.modelSpace())
        if (id != arr && entityAs<InsertData>(*f.doc.find(id)))
            arr2 = id;
    REQUIRE(arr2);
    auto ex2 = blocks::explode(f.doc, *f.doc.find(arr2));
    REQUIRE(ex2.ok());
    REQUIRE(ex2.value().entities.size() == 5);
    CHECK(near(entityAs<CircleData>(ex2.value().entities[0])->center, {0, 20}, 1e-6));
    CHECK(near(entityAs<CircleData>(ex2.value().entities[4])->center, {30, 20}, 1e-6));
    double lastX = -1;
    for (const Entity& e : ex2.value().entities) {
        const double x = entityAs<CircleData>(e)->center.x;
        CHECK(x > lastX);
        lastX = x;
    }
}

TEST_CASE("ARRAYEDIT options: Columns with Total, Angle between, Divide and Measure, one undo") {
    {
        Fixture f;
        EntityId c = f.addCircle(0, 0, 1);
        f.host->setSelection({c});
        f.host->run("ARRAY", {"R", "Rows", "2", "Columns", "2", "Spacing", "5", "5", ""});
        EntityId arr = f.modelSpace()[0];
        const std::size_t steps = f.host->undoStack().undoCount();
        REQUIRE(f.host->run("ARRAYEDIT", {cmd::ScriptItem::pick(arr), "C", "5", "Total", "100",
                                          "R", "3", "", ""})
                    .finished);
        const ArrayInfo& a = *onlyInsert(f)->array;
        CHECK(a.columns == 5);
        CHECK(a.columnSpacing == doctest::Approx(25));
        CHECK(a.rows == 3);
        CHECK(a.rowSpacing == doctest::Approx(5));
        CHECK(f.host->undoStack().undoCount() == steps + 1);
        CHECK(f.host->undo());
        CHECK(onlyInsert(f)->array->columns == 2);
        CHECK(onlyInsert(f)->array->rows == 2);
    }
    {
        Fixture f;
        EntityId c = f.addCircle(10, 0, 1);
        f.host->setSelection({c});
        f.host->run("ARRAYPOLAR", {"0,0", "Items", "4", "Angle", "90", ""});
        EntityId arr = f.modelSpace()[0];
        // 30 degrees between 4 items fills 90; too wide an angle is refused
        REQUIRE(f.host->run("ARRAYEDIT", {cmd::ScriptItem::pick(arr), "A", "20", "A", "200", ""})
                    .finished);
        CHECK(onlyInsert(f)->array->fillAngle == doctest::Approx(60 * kPi / 180));
        CHECK(f.host->logContains("more than once"));
    }
    {
        Fixture f;
        EntityId c = f.addCircle(0, 0, 1);
        EntityId path = f.addLine(0, 0, 20, 0);
        f.host->setSelection({c});
        f.host->run("ARRAYPATH", {cmd::ScriptItem::pick(path), "", "", "5", "N"});
        EntityId arr;
        for (EntityId id : f.modelSpace())
            if (entityAs<InsertData>(*f.doc.find(id)))
                arr = id;
        // Measure keeps the spacing Divide gave (20 / 4 = 5), then Items asks for it
        REQUIRE(f.host->run("ARRAYEDIT", {cmd::ScriptItem::pick(arr), "M", "Measure", "I", "2",
                                          "3", ""})
                    .finished);
        const InsertData* ins = nullptr;
        f.doc.forEachEntity([&](const Entity& e) {
            if (const auto* i = entityAs<InsertData>(e))
                ins = i;
        });
        REQUIRE(ins);
        CHECK(ins->array->pathSpacing == doctest::Approx(2));
        CHECK(ins->array->count == 3);
        REQUIRE(f.host->run("ARRAYEDIT", {cmd::ScriptItem::pick(arr), "M", "D", ""}).finished);
        f.doc.forEachEntity([&](const Entity& e) {
            if (const auto* i = entityAs<InsertData>(e))
                ins = i;
        });
        CHECK(ins->array->pathSpacing == 0);
    }
}

TEST_CASE("array grips change the count and spacing of rectangular and polar arrays") {
    {
        Fixture f;
        EntityId c = f.addCircle(0, 0, 1);
        f.host->setSelection({c});
        f.host->run("ARRAY", {"R", "Rows", "2", "Columns", "3", "Spacing", "10", "5", ""});
        Entity e = *f.doc.find(f.modelSpace()[0]);
        const auto g = geom::gripPoints(e);
        REQUIRE(g.size() == 5);
        CHECK(near(g[1], {20, 0}));  // last column (columns 10 apart, the base at the origin)
        CHECK(near(g[2], {10, 0}));  // second column
        CHECK(near(g[3], {0, 5}));   // last row (rows 5 apart)
        CHECK(near(g[4], {0, 5}));   // second row (two rows: the same place)
        auto more = geom::moveGrip(e, 1, {52, 2});  // columns up to x = 50 at 10 apart: 6
        REQUIRE(more.ok());
        CHECK(entityAs<InsertData>(more.value())->array->columns == 6);
        auto wider = geom::moveGrip(e, 2, {8, 0});
        REQUIRE(wider.ok());
        CHECK(entityAs<InsertData>(wider.value())->array->columnSpacing == doctest::Approx(8));
        CHECK_FALSE(geom::moveGrip(e, 4, {3, 0}).ok());  // zero row spacing is refused
        // a moved and turned array keeps its grips on the items
        InsertData& d = *entityAs<InsertData>(e);
        d.position = {100, 0};
        d.rotation = kPi / 2;
        CHECK(near(geom::gripPoints(e)[2], {100, 10}));
        auto rows = geom::moveGrip(e, 3, {70, 0});  // rows run along -x after the turn: 30 / 5
        REQUIRE(rows.ok());
        CHECK(entityAs<InsertData>(rows.value())->array->rows == 7);
    }
    {
        Fixture f;
        EntityId c = f.addCircle(10, 0, 1);
        f.host->setSelection({c});
        f.host->run("ARRAYPOLAR", {"0,0", "Items", "4", "Angle", "90", ""});
        Entity e = *f.doc.find(f.modelSpace()[0]);
        const auto g = geom::gripPoints(e);
        REQUIRE(g.size() == 3);
        CHECK(near(g[2], {0, 10}));  // the last item at 90 degrees
        auto fill = geom::moveGrip(e, 2, {-10, 0});
        REQUIRE(fill.ok());
        CHECK(entityAs<InsertData>(fill.value())->array->fillAngle == doctest::Approx(kPi));
        auto step = geom::moveGrip(e, 1, {0, 10});  // 90 between 4 items: 270 filled
        REQUIRE(step.ok());
        CHECK(entityAs<InsertData>(step.value())->array->fillAngle ==
              doctest::Approx(3 * kPi / 2));
    }
}
