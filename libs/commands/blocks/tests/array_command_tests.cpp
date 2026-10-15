#include "support.h"

using namespace tbcad;
using namespace tbcad::block_cmds;
using namespace tbcad::block_cmds::test;

TEST_CASE("ARRAY rectangular creates the grid of copies") {
    Fixture f;
    EntityId c = f.addCircle(0, 0, 1);
    f.host->setSelection({c});
    // rows 2, columns 3, column spacing 5, row spacing 10, angle 0
    auto r = f.host->run("ARRAY", {"R", "2", "3", "5", "10", "0", "N"});
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
    auto r = f.host->run("ARRAY", {"PO", "0,0", "6", "360", "Y", "N"});
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
    f.host->run("ARRAY", {"", "", "", "", "", "", "N"});
    CHECK(f.modelSpace().size() == 12);  // 3 rows x 4 columns
}

TEST_CASE("ARRAY refuses a huge array and Esc changes nothing") {
    Fixture f;
    EntityId c = f.addCircle(0, 0, 1);
    f.host->setSelection({c});
    f.host->run("ARRAY", {"R", "5000", "5000", "5", "5", "0"});
    CHECK(f.modelSpace().size() == 1);
    f.host->setSelection({c});
    f.host->run("ARRAY", {"R", "2", cmd::ScriptItem::esc()});
    CHECK(f.modelSpace().size() == 1);
}

TEST_CASE("ARRAY previews the copies while the last distance is chosen") {
    Fixture f;
    EntityId c = f.addCircle(0, 0, 1);
    f.host->setSelection({c});
    f.host->run("ARRAY", {"R", "2", "2", "5"});
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
    auto r = f.host->run("ARRAY", {"R", "2", "3", "5", "10", "0", ""});
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
    f.host->run("ARRAYPOLAR", {"0,0", "4", "360", "Y", ""});
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
    f.host->run("ARRAY", {"R", "2", "2", "5", "5", "0", "Y"});
    EntityId arr = f.modelSpace()[0];
    f.host->run("ARRAYEDIT", {cmd::ScriptItem::pick(arr), "4", "", "7", ""});
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
    auto r = f.host->run("ARRAY", {"R"});  // now at "number of rows"
    CHECK(!r.finished);
    f.host->runner().textEdited("2");
    CHECK(!f.host->preview().items.empty());
    const std::size_t two = f.host->preview().items.size();
    f.host->runner().textEdited("4");
    CHECK(f.host->preview().items.size() > two);
    f.host->runner().textEdited("");
    CHECK(f.host->preview().items.empty());
}
