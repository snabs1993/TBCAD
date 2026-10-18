#include <doctest/doctest.h>

#include <cmath>

#include "tbcad/cmd/cmd.h"
#include "tbcad/cmd/register_all.h"
#include "tbcad/text/text.h"

using namespace tbcad;

namespace {

struct Fixture {
    Document doc;
    cmd::Registry reg;
    cmd::ScriptHost host;
    Fixture() : host(doc, (registerAllCommands(reg), reg)) {}

    const TextData* firstText() {
        const TextData* r = nullptr;
        doc.forEachEntity([&](const Entity& e) {
            if (!r)
                r = entityAs<TextData>(e);
        });
        return r;
    }
    const MTextData* firstMText() {
        const MTextData* r = nullptr;
        doc.forEachEntity([&](const Entity& e) {
            if (!r)
                r = entityAs<MTextData>(e);
        });
        return r;
    }
};

const std::string kHong = "\u0E2B\u0E49\u0E2D\u0E07";  // ห้อง

}  // namespace

TEST_CASE("TEXT: start point, height, rotation, text") {
    Fixture f;
    auto r = f.host.run("TEXT", {"10,20", "5", "90", "Hello"});
    CHECK(r.finished);
    const TextData* t = f.firstText();
    REQUIRE(t);
    CHECK(t->position == Vec2{10, 20});
    CHECK(t->alignPoint == Vec2{10, 20});
    CHECK(t->height == doctest::Approx(5));
    CHECK(t->rotation == doctest::Approx(kPi / 2));
    CHECK(t->utf8 == "Hello");
    CHECK(t->h == HAlign::Left);
    CHECK(t->v == VAlign::Baseline);
    CHECK(f.host.undoStack().undoLabel() == "TEXT");
    f.host.undo();
    CHECK(f.doc.entityCount() == 0);
}

TEST_CASE("TEXT accepts Thai text") {
    Fixture f;
    f.host.run("T", {"0,0", "", "", kHong});
    const TextData* t = f.firstText();
    REQUIRE(t);
    CHECK(t->utf8 == kHong);
    CHECK(t->height == doctest::Approx(2.5));  // Enter takes the default height
}

TEST_CASE("TEXT remembers the last height as the next default") {
    Fixture f;
    f.host.run("TEXT", {"0,0", "7", "0", "a"});
    f.host.run("TEXT", {"1,1", "", "0", "b"});
    int n = 0;
    f.doc.forEachEntity([&](const Entity& e) {
        if (const auto* t = entityAs<TextData>(e)) {
            CHECK(t->height == doctest::Approx(7));
            ++n;
        }
    });
    CHECK(n == 2);
}

TEST_CASE("TEXT in a big drawing or at a set annotation scale defaults to readable height") {
    Fixture f;
    f.host.run("LINE", {"0,0", "20000,0", ""});  // a 20 m plan
    f.host.run("TEXT", {"0,0", "", "0", "a"});
    CHECK(f.firstText()->height == doctest::Approx(125));  // 2.5 * 50
    // The automatic height is not remembered as a user choice.
    CHECK(f.host.setting("text.height").value_or("").empty());

    Fixture g;
    g.host.run("CANNOSCALE", {"20"});
    g.host.run("TEXT", {"0,0", "", "0", "b"});
    CHECK(g.firstText()->height == doctest::Approx(50));
}

TEST_CASE("TEXT Justify options") {
    Fixture f;
    f.host.run("TEXT", {"J", "MC", "5,5", "4", "0", "mid"});
    const TextData* t = f.firstText();
    REQUIRE(t);
    CHECK(t->h == HAlign::Middle);
    CHECK(t->v == VAlign::Middle);

    Fixture g;
    g.host.run("TEXT", {"J", "TR", "5,5", "4", "0", "tr"});
    CHECK(g.firstText()->h == HAlign::Right);
    CHECK(g.firstText()->v == VAlign::Top);

    Fixture h;  // Aligned: second point, no height and no rotation prompts
    auto r = h.host.run("TEXT", {"J", "A", "0,0", "10,0", "fit me"});
    CHECK(r.finished);
    const TextData* a = h.firstText();
    REQUIRE(a);
    CHECK(a->h == HAlign::Aligned);
    CHECK(a->position == Vec2{0, 0});
    CHECK(a->alignPoint == Vec2{10, 0});
    CHECK(a->utf8 == "fit me");

    Fixture k;  // Fit asks for the second point and the height
    k.host.run("TEXT", {"J", "F", "0,0", "10,0", "3", "fit"});
    REQUIRE(k.firstText());
    CHECK(k.firstText()->h == HAlign::Fit);
    CHECK(k.firstText()->height == doctest::Approx(3));
}

TEST_CASE("TEXT Style option picks an existing style, unknown names are reported") {
    Fixture f;
    f.host.run("STYLE", {"Tall", "Sarabun", "0.5", "0", "0"});
    // STYLE made "Tall" current; pick Standard explicitly for this text.
    f.host.run("TEXT", {"S", "Nope", "S", "Standard", "0,0", "2", "0", "x"});
    CHECK(f.host.logContains("Unknown text style: Nope"));
    const TextData* t = f.firstText();
    REQUIRE(t);
    CHECK(t->style == kTextStyleStandard);
}

TEST_CASE("TEXT: Esc anywhere leaves the drawing untouched") {
    Fixture f;
    f.host.run("TEXT", {"0,0", "5", cmd::ScriptItem::esc()});
    CHECK(f.doc.entityCount() == 0);
    f.host.run("TEXT", {cmd::ScriptItem::esc()});
    CHECK(f.doc.entityCount() == 0);
}

TEST_CASE("MTEXT: corners give width and top-left insertion point") {
    Fixture f;
    auto r = f.host.run("MT", {"H", "3", "0,100", "40,60", "line one\\Pline two"});
    CHECK(r.finished);
    const MTextData* m = f.firstMText();
    REQUIRE(m);
    CHECK(m->position == Vec2{0, 100});
    CHECK(m->width == doctest::Approx(40));
    CHECK(m->height == doctest::Approx(3));
    CHECK(m->attach == Attachment::TopLeft);
    CHECK(m->utf8 == "line one\\Pline two");
}

TEST_CASE("MTEXT, TEXT and TEXTEDIT ask for a text box where the text goes") {
    Fixture f;
    f.host.run("MT", {"0,100", "40,60"});  // stops at the text prompt
    REQUIRE(f.host.textEditor());
    CHECK(f.host.textEditor()->anchor == Vec2{0, 100});
    CHECK(f.host.textEditor()->widthMm == doctest::Approx(40));
    CHECK(f.host.textEditor()->heightMm == doctest::Approx(40));
    CHECK(f.host.textEditor()->multiline);
    f.host.run("", {cmd::ScriptItem::esc()});

    Fixture g;
    g.host.run("TEXT", {"10,10", "2.5", "0"});
    REQUIRE(g.host.textEditor());
    CHECK_FALSE(g.host.textEditor()->multiline);
    CHECK(g.host.textEditor()->anchor == Vec2{10, 12.5});
}

TEST_CASE("MTEXT: Justify, Width, Rotation options and corner order") {
    Fixture f;
    f.host.run("MTEXT", {"J", "BR", "W", "25", "10,10", "50,0", "x"});
    const MTextData* m = f.firstMText();
    REQUIRE(m);
    CHECK(m->attach == Attachment::BottomRight);
    CHECK(m->width == doctest::Approx(25));
    // The bottom-right of the rectangle (10,10)-(50,0) is (50,0).
    CHECK(m->position.x == doctest::Approx(50));
    CHECK(m->position.y == doctest::Approx(0));

    Fixture g;
    g.host.run("MTEXT", {"R", "90", "0,0", "0,30", "rot"});
    const MTextData* r = g.firstMText();
    REQUIRE(r);
    CHECK(r->rotation == doctest::Approx(kPi / 2));
    CHECK(r->width == doctest::Approx(30));  // measured along the rotated x axis
    CHECK(r->position.x == doctest::Approx(0).epsilon(1e-9));
    CHECK(r->position.y == doctest::Approx(0).epsilon(1e-9));
}

TEST_CASE("MTEXT: Justify chosen in the text box moves the text to that point of the rectangle") {
    Fixture f;
    f.host.run("MTEXT", {"10,10", "50,0"});  // now waiting for the text
    f.host.setSetting("tmp.mtext.attach", "MC");
    f.host.setSetting("tmp.mtext.height", "5");
    f.host.feed({"x"});
    const MTextData* m = f.firstMText();
    REQUIRE(m);
    CHECK(m->attach == Attachment::MiddleCenter);
    CHECK(m->height == doctest::Approx(5));
    CHECK(m->position.x == doctest::Approx(30));
    CHECK(m->position.y == doctest::Approx(5));
}

TEST_CASE("MTEXT shows a rectangle preview while picking the second corner") {
    Fixture f;
    f.host.run("MTEXT", {"0,0"});
    f.host.runner().pointerMoved(cmd::RawInput{{10, -5}, {10, -5}, 1});
    CHECK_FALSE(f.host.preview().items.empty());
    f.host.feed({cmd::ScriptItem::esc()});
}

TEST_CASE("TEXTEDIT edits TEXT and MTEXT in one undo step") {
    Fixture f;
    f.host.run("TEXT", {"0,0", "5", "0", "before"});
    EntityId id = f.doc.modelSpace().front();
    f.host.setSelection({id});
    auto r = f.host.run("ED", {"after"});
    CHECK(r.finished);
    CHECK(f.firstText()->utf8 == "after");
    CHECK(f.host.undoStack().undoLabel() == "TEXTEDIT");
    f.host.undo();
    CHECK(f.firstText()->utf8 == "before");

    // Enter keeps the current text; Thai content replaces it.
    f.host.setSelection({id});
    f.host.run("TEXTEDIT", {""});
    CHECK(f.firstText()->utf8 == "before");
    f.host.setSelection({id});
    f.host.run("TEXTEDIT", {kHong});
    CHECK(f.firstText()->utf8 == kHong);

    Fixture g;
    g.host.run("MTEXT", {"0,0", "20,-10", "a"});
    g.host.setSelection({g.doc.modelSpace().front()});
    g.host.run("TEXTEDIT", {"{\\b1;bold}\\Pnext"});
    CHECK(g.firstMText()->utf8 == "{\\b1;bold}\\Pnext");
}

TEST_CASE("TEXTEDIT ignores objects that are not text") {
    Fixture f;
    auto tx = f.doc.begin("line");
    auto id = tx.add(Entity{{}, LineData{{0, 0}, {10, 0}}});
    tx.commit();
    REQUIRE(id.ok());
    f.host.setSelection({id.value()});
    f.host.run("TEXTEDIT", {cmd::ScriptItem::esc()});
    CHECK(f.doc.entityCount() == 1);
}

TEST_CASE("created text displays: Thai TEXT produces filled rings through the registry") {
    Fixture f;
    f.host.run("TEXT", {"0,0", "10", "0", kHong});
    const Entity* e = f.doc.find(f.doc.modelSpace().front());
    REQUIRE(e);
    geom::DisplayContext ctx;
    ctx.pixelSizeMm = 0.01;
    DisplayList out;
    geom::buildDisplay(f.doc, *e, ctx, out);
    CHECK(out.items.size() >= 3);
    CHECK(std::holds_alternative<DisplayFill>(out.items.front()));
}

TEST_CASE("TXT2MTXT merges lines top to bottom into one multiline text") {
    Fixture f;
    auto addText = [&](Vec2 at, const std::string& s) {
        TextData t;
        t.position = at;
        t.height = 2.5;
        t.utf8 = s;
        Transaction tx = f.doc.begin("setup");
        Entity e;
        e.data = Indirect<TextData>(std::move(t));
        EntityId id = tx.add(std::move(e)).value();
        tx.commit();
        return id;
    };
    EntityId low = addText({0, 0}, "second {x}");
    EntityId high = addText({0, 10}, "first");
    REQUIRE(f.host.run("TXT2MTXT", {cmd::ScriptItem::select({low, high}), cmd::ScriptItem::enter()})
                .finished);
    const MTextData* m = f.firstMText();
    REQUIRE(m);
    CHECK(m->utf8 == "first\\Psecond \\{x\\}");
    CHECK(m->position.y == doctest::Approx(12.5));
    CHECK(f.firstText() == nullptr);
    CHECK(f.host.undo());
    CHECK(f.firstMText() == nullptr);
}

TEST_CASE("TXTEXP turns text into closed polylines and undo brings the text back") {
    Fixture f;
    f.host.run("TEXT", {"0,0", "10", "0", "HOH"});
    REQUIRE(f.firstText() != nullptr);
    EntityId text = f.doc.modelSpace().front();
    REQUIRE(
        f.host.run("TXTEXP", {cmd::ScriptItem::select({text}), cmd::ScriptItem::enter()}).finished);
    CHECK(f.firstText() == nullptr);
    std::size_t closed = 0;
    for (EntityId id : f.doc.modelSpace()) {
        const auto* pl = entityAs<PolylineData>(*f.doc.find(id));
        REQUIRE(pl != nullptr);
        CHECK(pl->closed);
        ++closed;
    }
    CHECK(closed >= 3);
    CHECK(f.host.undo());
    CHECK(f.firstText() != nullptr);
}

TEST_CASE("TCOUNT numbers text objects in selection order") {
    Fixture f;
    std::vector<EntityId> ids;
    for (int i = 0; i < 3; ++i) {
        TextData t;
        t.position = {10.0 * i, 0};
        t.utf8 = "x";
        Transaction tx = f.doc.begin("setup");
        Entity e;
        e.data = Indirect<TextData>(std::move(t));
        ids.push_back(tx.add(std::move(e)).value());
        tx.commit();
    }
    REQUIRE(f.host
                .run("TCOUNT", {cmd::ScriptItem::select({ids[2], ids[0], ids[1]}),
                                cmd::ScriptItem::enter(), "10", "5", "P-", ""})
                .finished);
    CHECK(entityAs<TextData>(*f.doc.find(ids[2]))->utf8 == "P-10");
    CHECK(entityAs<TextData>(*f.doc.find(ids[0]))->utf8 == "P-15");
    CHECK(entityAs<TextData>(*f.doc.find(ids[1]))->utf8 == "P-20");
}

TEST_CASE("SPELL flags English typos, skips Thai, and Add remembers a word") {
    Fixture f;
    std::vector<EntityId> ids;
    const std::string thai = kHong + "xyzzy " + kHong;  // glued to Thai: never asked about
    for (const std::string& s :
         {std::string("The kichen wall"), thai, std::string("TBCAD 12mm"), std::string("zorbel")}) {
        TextData t;
        t.utf8 = s;
        Transaction tx = f.doc.begin("setup");
        Entity e;
        e.data = Indirect<TextData>(std::move(t));
        ids.push_back(tx.add(std::move(e)).value());
        tx.commit();
    }
    REQUIRE(f.host
                .run("SPELL", {cmd::ScriptItem::select(ids), cmd::ScriptItem::enter(), "Change",
                               "kitchen", "Add"})
                .finished);
    CHECK(entityAs<TextData>(*f.doc.find(ids[0]))->utf8 == "The kitchen wall");
    CHECK(entityAs<TextData>(*f.doc.find(ids[1]))->utf8 == thai);
    CHECK(f.doc.header().custom.count("tbcad.spell.words") == 1);
    // Added words are accepted next time: nothing is asked.
    CHECK(f.host.run("SPELL", {cmd::ScriptItem::select(ids), cmd::ScriptItem::enter()}).finished);
}

TEST_CASE("MTEXT and TEXT from far away default to a height that can be read") {
    Fixture f;
    f.host.setView({{0, 0}, 10.0, {900, 600}});  // 10 mm per pixel: a plan in millimetres
    f.host.run("MT", {"0,1000", "4000,500", "far"});
    const MTextData* m = f.firstMText();
    REQUIRE(m);
    CHECK(m->height == doctest::Approx(125));  // 2.5 mm paper text at 1:50
    // A normal zoom keeps 2.5 mm.
    Fixture g;
    g.host.run("MT", {"0,100", "40,60", "near"});
    REQUIRE(g.firstMText());
    CHECK(g.firstMText()->height == doctest::Approx(2.5));
}

TEST_CASE("TEXTALIGN lines texts up with the first one and undoes in one step") {
    Fixture f;
    f.host.run("TEXT", {"10,5", "2.5", "0", "AAA"});
    f.host.run("TEXT", {"40,20", "2.5", "0", "B"});
    REQUIRE(f.doc.modelSpace().size() == 2);
    const EntityId ta = f.doc.modelSpace()[0];
    const EntityId tb = f.doc.modelSpace()[1];
    f.host.run("TEXTALIGN", {cmd::ScriptItem::select({ta, tb}), cmd::ScriptItem::enter(), "Left"});
    CHECK(entityAs<TextData>(*f.doc.find(tb))->position.x == doctest::Approx(10));
    CHECK(entityAs<TextData>(*f.doc.find(tb))->position.y == doctest::Approx(20));
    f.host.run("TEXTALIGN",
               {cmd::ScriptItem::select({ta, tb}), cmd::ScriptItem::enter(), "Bottom"});
    CHECK(entityAs<TextData>(*f.doc.find(tb))->position.y == doctest::Approx(5).epsilon(0.5));
    f.host.undo();
    CHECK(entityAs<TextData>(*f.doc.find(tb))->position.y == doctest::Approx(20));
}

TEST_CASE("ARCTEXT puts one text object per letter along the arc") {
    Fixture f;
    EntityId arc;
    {
        Transaction tx = f.doc.begin("setup");
        Entity e;
        e.data = ArcData{{0, 0}, 100, 0, 3.141592653589793};
        arc = tx.add(std::move(e)).value();
        tx.commit();
    }
    REQUIRE(f.host.run("ARCTEXT", {cmd::ScriptItem::pick(arc, {0, 100}), "AB C", "5", "Outside"})
                .finished);
    std::vector<const TextData*> texts;
    for (EntityId id : f.doc.modelSpace())
        if (const auto* t = entityAs<TextData>(*f.doc.find(id)))
            texts.push_back(t);
    REQUIRE(texts.size() == 3);  // the space gets none
    // Outside text on the top half reads left to right: x grows, and the middle letters are
    // upright.
    CHECK(texts[0]->position.x < texts[1]->position.x);
    CHECK(texts[1]->position.x < texts[2]->position.x);
    CHECK(texts[0]->position.y > 90);
    CHECK(std::abs(texts[1]->rotation) < 0.3);
    CHECK(f.host.undo());
    CHECK(f.doc.modelSpace().size() == 1);
}
