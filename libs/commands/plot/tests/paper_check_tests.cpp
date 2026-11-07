#include <doctest/doctest.h>

#include "tbcad/cmd/cmd.h"
#include "tbcad/layout/paper_size.h"
#include "tbcad/layout/plotframes.h"
#include "tbcad/plot/plot.h"

using namespace tbcad;

namespace {

EntityId addText(Document& doc, Vec2 at, double height) {
    TextData t;
    t.position = at;
    t.height = height;
    t.utf8 = "ROOM";
    Entity e;
    e.data = Indirect<TextData>(std::move(t));
    auto tx = doc.begin("text");
    auto r = tx.add(std::move(e));
    REQUIRE(r.ok());
    tx.commit();
    return r.value();
}

double heightOf(const Document& doc, EntityId id) {
    return entityAs<TextData>(*doc.find(id))->height;
}

}  // namespace

TEST_CASE("PAPERCHECK reports, PAPERFIT fits the selection or the whole drawing in one undo") {
    Document doc;
    cmd::Registry reg;
    plot::registerCommands(reg);
    {
        layout::PlotFrame f;
        f.name = "Plan";
        f.window.expand({0, 0});
        f.window.expand({84100, 59400});
        f.paperName = "A1";
        f.scaleN = 100;
        auto tx = doc.begin("frame");
        REQUIRE(layout::savePlotFrame(tx, f).ok());
        tx.commit();
    }
    const EntityId good = addText(doc, {1000, 1000}, 250);
    const EntityId old = addText(doc, {2000, 1000}, 125);
    const EntityId tiny = addText(doc, {3000, 1000}, 60);
    cmd::ScriptHost host(doc, reg);

    REQUIRE(host.run("PAPERCHECK", {}).finished);
    CHECK(
        host.logContains("whole drawing): 3 checked: 1 OK, 1 made for another scale, 1 too small"));

    // Only the selected one.
    host.setSelection({old});
    REQUIRE(host.run("PAPERFIT", {"All"}).finished);
    CHECK(heightOf(doc, old) == doctest::Approx(250));
    CHECK(heightOf(doc, tiny) == doctest::Approx(60));
    CHECK(host.selection().empty());

    // Everything left, then one undo puts it back.
    REQUIRE(host.run("PAPERFIT", {"Small"}).finished);
    CHECK(heightOf(doc, tiny) == doctest::Approx(180));
    CHECK(heightOf(doc, good) == doctest::Approx(250));
    REQUIRE(host.undo());
    CHECK(heightOf(doc, tiny) == doctest::Approx(60));
}
