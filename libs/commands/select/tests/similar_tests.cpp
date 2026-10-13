#include "test_util.h"

using namespace tbcad;
using namespace tbcad::select;
using namespace tbcad::select::test;
using cmd::ScriptItem;

TEST_CASE("similarEntities: same type and layer by default mask") {
    Fixture f;
    LayerId a = f.addLayer("A");
    auto l1 = f.addEntity(LineData{{0, 0}, {1, 0}}, a);
    auto l2 = f.addEntity(LineData{{0, 1}, {1, 1}}, a);
    f.addEntity(LineData{{0, 2}, {1, 2}});  // other layer
    f.addEntity(CircleData{{0, 0}, 1}, a);  // other type
    auto ids =
        similarEntities(f.doc, *f.doc.find(l1),
                        static_cast<unsigned>(Match::Type) | static_cast<unsigned>(Match::Layer));
    CHECK(ids.size() == 2);
    CHECK(has(ids, l1));
    CHECK(has(ids, l2));
}

TEST_CASE("similarEntities: colour match compares what is drawn") {
    Fixture f;
    auto a = f.addEntity(LineData{{0, 0}, {1, 0}});
    auto b = f.addEntity(LineData{{0, 1}, {1, 1}});
    f.modify(b, [](Entity& e) { e.common.color = Color::index(1); });
    auto ids = similarEntities(f.doc, *f.doc.find(a), static_cast<unsigned>(Match::Color));
    CHECK(ids.size() == 1);
}

TEST_CASE("SELECTSIMILAR selects from a picked object") {
    Fixture f;
    auto l1 = f.addEntity(LineData{{0, 0}, {1, 0}});
    f.addEntity(LineData{{0, 1}, {1, 1}});
    f.addEntity(CircleData{{0, 0}, 1});
    f.host.run("SELSIM", {ScriptItem::pick(l1)});
    CHECK(f.host.selection().size() == 2);
    CHECK(f.host.logContains("2 object(s) found matching Type, Layer."));
}

TEST_CASE("SELECTSIMILAR Settings toggles what is matched and is remembered") {
    Fixture f;
    auto l1 = f.addEntity(LineData{{0, 0}, {1, 0}});
    auto l2 = f.addEntity(LineData{{0, 1}, {1, 1}});
    f.modify(l2, [](Entity& e) { e.common.color = Color::index(1); });
    f.host.run("SELECTSIMILAR", {"Settings", "Color", "Done", ScriptItem::pick(l1)});
    CHECK(f.host.selection().size() == 1);  // colour now has to match too
    CHECK(f.host.logContains("Type, Layer, Color"));
}
