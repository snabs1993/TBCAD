#include <doctest/doctest.h>

#include "tbcad/cmd/cmd.h"
#include "tbcad/cmd/register_all.h"
#include "tbcad/geom/entity.h"

using namespace tbcad;

namespace {
struct Fixture {
    Document doc;
    cmd::Registry reg;
    cmd::ScriptHost host;
    Fixture() : host(doc, (registerAllCommands(reg), reg)) {}
};

double textHeightOnScreen(const Document& doc, const Entity& e, double annoScale = 0) {
    geom::DisplayContext ctx;
    ctx.annoScale = annoScale;
    DisplayList d;
    geom::buildDisplay(doc, e, ctx, d);
    double top = -1e30, bot = 1e30;
    auto take = [&](const std::vector<Vec2>& pts) {
        for (Vec2 v : pts) {
            top = std::max(top, v.y);
            bot = std::min(bot, v.y);
        }
    };
    for (const auto& item : d.items) {
        if (const auto* p = std::get_if<DisplayPolyline>(&item))
            take(p->pts);
        else if (const auto* f = std::get_if<DisplayFill>(&item))
            for (const auto& r : f->rings)
                take(r);
    }
    return top - bot;
}
}  // namespace

TEST_CASE("annotation scale list and current scale are stored in the drawing") {
    Fixture f;
    CHECK(currentAnnotationScale(f.doc.header()) == doctest::Approx(1));
    f.host.run("CANNOSCALE", {"50"});
    CHECK(currentAnnotationScale(f.doc.header()) == doctest::Approx(50));
    f.host.undo();
    CHECK(currentAnnotationScale(f.doc.header()) == doctest::Approx(1));

    f.host.run("SCALELISTEDIT", {"Add", "40", "Delete", "1000", "List", "Done"});
    auto list = annotationScaleList(f.doc.header());
    CHECK(std::find(list.begin(), list.end(), 40.0) != list.end());
    CHECK(std::find(list.begin(), list.end(), 1000.0) == list.end());
    f.host.run("SCALELISTEDIT", {"Reset", "Done"});
    CHECK(annotationScaleList(f.doc.header()).size() == 13);
}

TEST_CASE("annotative text grows with the scale; ordinary text does not") {
    Fixture f;
    f.host.run("TEXT", {"0,0", "2.5", "0", "Hi"});
    f.host.run("TEXT", {"0,10", "2.5", "0", "Hi"});
    std::vector<EntityId> ids;
    f.doc.forEachEntity([&](const Entity& e) { ids.push_back(e.common.id); });
    std::sort(ids.begin(), ids.end());
    REQUIRE(ids.size() == 2);
    f.host.run("ANNOTATIVE",
               {cmd::ScriptItem::pick(ids[0], {0, 0}), cmd::ScriptItem::enter(), "On"});
    CHECK(f.doc.find(ids[0])->common.annotative);
    CHECK_FALSE(f.doc.find(ids[1])->common.annotative);

    const double h1 = textHeightOnScreen(f.doc, *f.doc.find(ids[0]));
    f.host.run("CANNOSCALE", {"100"});
    const double h100 = textHeightOnScreen(f.doc, *f.doc.find(ids[0]));
    CHECK(h100 == doctest::Approx(h1 * 100).epsilon(0.01));
    CHECK(textHeightOnScreen(f.doc, *f.doc.find(ids[1])) == doctest::Approx(h1).epsilon(0.01));
    // A viewport overrides the current scale.
    CHECK(textHeightOnScreen(f.doc, *f.doc.find(ids[0]), 10) ==
          doctest::Approx(h1 * 10).epsilon(0.01));
}

TEST_CASE("an object keeps its own scales; new objects follow ANNONEW") {
    Fixture f;
    f.host.run("TEXT", {"0,0", "2.5", "0", "Hi"});
    EntityId id{};
    f.doc.forEachEntity([&](const Entity& e) { id = e.common.id; });
    const double h1 = textHeightOnScreen(f.doc, *f.doc.find(id));
    f.host.run("ANNOTATIVE",
               {cmd::ScriptItem::pick(id, {0, 0}), cmd::ScriptItem::enter(), "Scales", "50,100"});
    const Entity* e = f.doc.find(id);
    REQUIRE(e);
    CHECK(e->common.annotative);
    REQUIRE(e->common.annoScales.size() == 2);
    // Drawn at its first scale whatever the drawing's current scale is.
    f.host.run("CANNOSCALE", {"200"});
    CHECK(textHeightOnScreen(f.doc, *f.doc.find(id)) == doctest::Approx(h1 * 50).epsilon(0.01));
    f.host.undo();
    f.host.undo();

    f.host.run("ANNONEW", {"On"});
    f.host.run("CANNOSCALE", {"20"});
    f.host.run("TEXT", {"0,10", "2.5", "0", "New"});
    EntityId made{};
    f.doc.forEachEntity([&](const Entity& x) {
        if (x.common.id != id)
            made = x.common.id;
    });
    REQUIRE(made != EntityId{});
    const Entity* n = f.doc.find(made);
    CHECK(n->common.annotative);
    REQUIRE(n->common.annoScales.size() == 1);
    CHECK(n->common.annoScales.front() == doctest::Approx(20));
}

TEST_CASE("a symbol is rescaled when its annotation scale changes") {
    Fixture f;
    f.host.run("LINE", {"0,0", "1,0", ""});
    EntityId line{};
    f.doc.forEachEntity([&](const Entity& e) { line = e.common.id; });
    f.host.setSelection({line});
    f.host.run("BLOCK", {"Mark", "0,0", "D"});
    f.host.run("INSERT", {"Mark", "10,0"});
    EntityId ins{};
    f.doc.forEachEntity([&](const Entity& e) {
        if (kind(e) == EntityKind::Insert)
            ins = e.common.id;
    });
    REQUIRE(ins != EntityId{});
    CHECK(entityAs<InsertData>(*f.doc.find(ins))->scale.x == doctest::Approx(1));
    f.host.setSelection({ins});
    f.host.run("ANNOTATIVE", {"Scales", "50"});
    CHECK(entityAs<InsertData>(*f.doc.find(ins))->scale.x == doctest::Approx(50));
    f.host.setSelection({ins});
    f.host.run("ANNOTATIVE", {"Scales", "100"});
    CHECK(entityAs<InsertData>(*f.doc.find(ins))->scale.x == doctest::Approx(100));
    f.host.setSelection({ins});
    f.host.run("ANNOTATIVE", {"Off"});
    CHECK(entityAs<InsertData>(*f.doc.find(ins))->scale.x == doctest::Approx(1));
}
