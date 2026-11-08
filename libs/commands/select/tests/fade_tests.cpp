#include "test_util.h"

using namespace tbcad;
using namespace tbcad::select;
using namespace tbcad::select::test;
using cmd::ScriptItem;

namespace {
bool faded(const Fixture& f, EntityId id) {
    return f.doc.find(id)->common.faded;
}
}  // namespace

TEST_CASE("FADE fades only the picked objects and FADE again brings them back") {
    Fixture f;
    auto a = f.addEntity(LineData{{0, 0}, {1, 0}});
    auto b = f.addEntity(LineData{{0, 1}, {1, 1}});
    auto c = f.addEntity(LineData{{0, 2}, {1, 2}});
    f.host.run("FADE", {ScriptItem::select({b}), ""});
    CHECK_FALSE(faded(f, a));
    CHECK(faded(f, b));
    CHECK_FALSE(faded(f, c));
    CHECK(f.host.logContains("1 object(s) faded"));
    f.host.run("FADE", {ScriptItem::select({b}), ""});
    CHECK_FALSE(faded(f, b));
    CHECK(f.host.logContains("1 object(s) shown in their own colours again"));
}

TEST_CASE("FADE on a mix fades them all - it is one undo step") {
    Fixture f;
    auto a = f.addEntity(LineData{{0, 0}, {1, 0}});
    auto b = f.addEntity(LineData{{0, 1}, {1, 1}});
    f.modify(a, [](Entity& e) { e.common.faded = true; });
    f.host.run("FADE", {ScriptItem::select({a, b}), ""});
    CHECK(faded(f, a));
    CHECK(faded(f, b));
    f.host.undo();
    CHECK(faded(f, a));
    CHECK_FALSE(faded(f, b));
}
