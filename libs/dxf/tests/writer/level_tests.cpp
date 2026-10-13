// Levels through the exporter and back (build/levels-brief.md).
#include "../lexer/doctest_include.h"
#include "../lexer/test_util.h"
#include "tbcad/dxf/reader.h"
#include "tbcad/dxf/writer.h"

namespace tbcad::dxf {
namespace {

struct Out {
    std::string bytes{};
    std::unique_ptr<Document> back{};
};

Out roundTrip(const Document& doc, ExportOptions options = {}) {
    Out o;
    Report r1;
    auto out = exportDxf(doc, options, r1);
    REQUIRE(out.ok());
    o.bytes = out.value().bytes;
    Report r2;
    auto in = importDxf(testing::bytesOf(o.bytes), ImportOptions{}, r2);
    REQUIRE_MESSAGE(in.ok(), (in.ok() ? "" : in.error().message));
    o.back = std::move(in.value().document);
    return o;
}

EntityId addAt(Document& doc, LevelId level, Vec2 a, Vec2 b) {
    auto tx = doc.begin("line");
    Entity e;
    e.data = LineData{a, b};
    e.common.level = level;
    auto id = tx.add(std::move(e));
    REQUIRE(id.ok());
    tx.commit();
    return id.value();
}

}  // namespace

TEST_CASE("levels, their state and each object's level survive a DXF round trip") {
    Document doc;
    LevelId l2, l3;
    {
        auto tx = doc.begin("levels");
        REQUIRE(tx.modifyLevel(kLevelDefault, [](Level& l) { l.name = "Ground"; }).ok());
        Level up;
        up.name = "Floor ทดสอบ 2";
        up.elevation = 3200;
        up.locked = true;
        l2 = tx.addLevel(up).value();
        Level empty;
        empty.name = "Roof";
        empty.elevation = 6400;
        empty.visible = false;
        l3 = tx.addLevel(empty).value();
        REQUIRE(tx.modifyHeader([&](Header& h) {
                      h.currentLevel = l2;
                      h.levelView = LevelView::HideOthers;
                  }).ok());
        tx.commit();
    }
    // The same position on two levels.
    EntityId a = addAt(doc, kLevelDefault, {0, 0}, {10, 0});
    EntityId b = addAt(doc, l2, {0, 0}, {10, 0});

    Out rt = roundTrip(doc);
    const Document& back = *rt.back;
    REQUIRE(back.levels().size() == 3);
    const Level* g = back.levelByName("Ground");
    const Level* f = back.levelByName("Floor ทดสอบ 2");
    const Level* r = back.levelByName("Roof");
    REQUIRE((g && f && r));
    CHECK(g->id == kLevelDefault);
    CHECK(f->elevation == 3200);
    CHECK(f->locked);
    CHECK_FALSE(r->visible);
    CHECK(r->elevation == 6400);  // an empty level survives
    CHECK(back.header().currentLevel == f->id);
    CHECK(back.header().levelView == LevelView::HideOthers);
    // Entities come back in file order; find them by level.
    int onG = 0, onF = 0;
    back.forEachEntity([&](const Entity& e) {
        onG += e.common.level == g->id;
        onF += e.common.level == f->id;
    });
    CHECK(onG == 1);
    CHECK(onF == 1);
    (void)a;
    (void)b;
    (void)l3;
}

TEST_CASE("a drawing with one pristine level writes no level data and reads back on Level 1") {
    Document doc;
    addAt(doc, kLevelDefault, {0, 0}, {5, 5});
    Out rt = roundTrip(doc);
    CHECK(rt.bytes.find("level=") == std::string::npos);
    CHECK(rt.back->levels().size() == 1);
    rt.back->forEachEntity([&](const Entity& e) { CHECK(e.common.level == kLevelDefault); });
}

TEST_CASE("exporting one level writes a plain single-level drawing") {
    Document doc;
    LevelId l2;
    {
        auto tx = doc.begin("level");
        Level up;
        up.name = "Upper";
        l2 = tx.addLevel(up).value();
        tx.commit();
    }
    addAt(doc, kLevelDefault, {0, 0}, {10, 0});
    addAt(doc, l2, {0, 0}, {20, 0});
    addAt(doc, l2, {0, 5}, {20, 5});
    ExportOptions opt;
    opt.onlyLevel = l2;
    Out rt = roundTrip(doc, opt);
    CHECK(rt.bytes.find("level=") == std::string::npos);
    CHECK(rt.back->levels().size() == 1);
    CHECK(rt.back->modelSpaceCount() == 2);
}

TEST_CASE("a foreign DXF has everything on the first level") {
    // Our own export of a one-level drawing carries no level data, like any other program's file.
    Document doc;
    addAt(doc, kLevelDefault, {0, 0}, {5, 5});
    Out rt = roundTrip(doc);
    REQUIRE(rt.back->modelSpaceCount() == 1);
    CHECK(rt.back->header().currentLevel == kLevelDefault);
}

}  // namespace tbcad::dxf
