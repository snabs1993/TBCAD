#include "test_util.h"

using namespace tbcad;
using namespace tbcad::select;
using namespace tbcad::select::test;
using cmd::ScriptItem;

TEST_CASE("HIDE hides the selection and UNISOLATE brings it back") {
    Fixture f;
    auto a = f.addEntity(LineData{{0, 0}, {1, 0}});
    auto b = f.addEntity(LineData{{0, 1}, {1, 1}});
    f.host.run("HIDE", {ScriptItem::select({a}), ""});
    CHECK_FALSE(f.visible(a));
    CHECK(f.visible(b));
    CHECK(f.host.logContains("1 object(s) hidden"));
    f.host.run("UNISOLATE", {});
    CHECK(f.visible(a));
    CHECK(f.host.logContains("1 object(s) shown"));
    CHECK(hiddenByIsolate(f.doc).empty());
}

TEST_CASE("ISOLATE hides everything else; UNISOLATE restores only what it hid") {
    Fixture f;
    auto a = f.addEntity(LineData{{0, 0}, {1, 0}});
    auto b = f.addEntity(LineData{{0, 1}, {1, 1}});
    auto c = f.addEntity(LineData{{0, 2}, {1, 2}});
    f.modify(c, [](Entity& e) { e.common.visible = false; });  // hidden by the user already
    f.host.run("ISOLATE", {ScriptItem::select({a}), ""});
    CHECK(f.visible(a));
    CHECK_FALSE(f.visible(b));
    f.host.run("UNISOLATE", {});
    CHECK(f.visible(b));
    CHECK_FALSE(f.visible(c));  // not ours to show
}

TEST_CASE("ISOLATE twice isolates relative to the whole drawing") {
    Fixture f;
    auto a = f.addEntity(LineData{{0, 0}, {1, 0}});
    auto b = f.addEntity(LineData{{0, 1}, {1, 1}});
    f.host.run("ISOLATE", {ScriptItem::select({a}), ""});
    f.host.run("ISOLATE", {ScriptItem::select({b}), ""});
    CHECK_FALSE(f.visible(a));
    CHECK(f.visible(b));
    f.host.run("UNISOLATE", {});
    CHECK(f.visible(a));
    CHECK(f.visible(b));
}

TEST_CASE("ISOLATE is one undo step and the record survives undo") {
    Fixture f;
    auto a = f.addEntity(LineData{{0, 0}, {1, 0}});
    auto b = f.addEntity(LineData{{0, 1}, {1, 1}});
    f.host.run("ISOLATE", {ScriptItem::select({a}), ""});
    CHECK(hiddenByIsolate(f.doc).size() == 1);
    f.host.undo();
    CHECK(f.visible(b));
    CHECK(hiddenByIsolate(f.doc).empty());
}

TEST_CASE("UNISOLATE with nothing hidden says so") {
    Fixture f;
    f.host.run("UNISOLATE", {});
    CHECK(f.host.logContains("No objects are hidden"));
}
