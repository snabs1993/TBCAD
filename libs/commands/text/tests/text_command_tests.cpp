#include <doctest/doctest.h>

#include <cmath>

#include "tbcad/cmd/cmd.h"
#include "tbcad/cmd/register_all.h"
#include "tbcad/geom/entity.h"
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
    f.host.run("DT", {"0,0", "", "", kHong});
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
    f.host.run("-TEXTSTYLE", {"Tall", "Sarabun", "0.5", "0", "0"});
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

TEST_CASE("TEXTEDIT Multiple mode edits one object after another, Undo takes the last back") {
    Fixture f;
    f.host.run("TEXT", {"0,0", "5", "0", "one"});
    f.host.run("TEXT", {"0,20", "5", "0", "two"});
    const EntityId a = f.doc.modelSpace()[0], b = f.doc.modelSpace()[1];
    auto content = [&](EntityId id) { return entityAs<TextData>(*f.doc.find(id))->utf8; };
    // TEXTEDITMODE 0 (the default): the command asks for the next object until Enter.
    auto r =
        f.host.run("TEXTEDIT", {cmd::ScriptItem::pick(a), "uno", cmd::ScriptItem::pick(b), "dos"});
    CHECK_FALSE(r.finished);
    CHECK(content(a) == "uno");
    CHECK(content(b) == "dos");
    f.host.feed({"U"});  // the last edit goes back
    CHECK(content(b) == "two");
    f.host.feed({cmd::ScriptItem::enter()});
    CHECK(content(a) == "uno");
    CHECK(f.host.undoStack().undoLabel() == "TEXTEDIT");
    f.host.undo();  // the whole command is one step
    CHECK(content(a) == "one");

    // Mode Single ends after one edit and is remembered as TEXTEDITMODE 1.
    r = f.host.run("TEXTEDIT", {"M", "S", cmd::ScriptItem::pick(a), "single"});
    CHECK(r.finished);
    CHECK(content(a) == "single");
    CHECK(f.host.setting("textedit.mode").value_or("") == "1");
    f.host.run("TEXTEDITMODE", {"0"});
    CHECK(f.host.setting("textedit.mode").value_or("") == "0");

    // An object selected first (a double-click) is a single edit whatever the mode.
    f.host.setSelection({b});
    r = f.host.run("TEXTEDIT", {"pre"});
    CHECK(r.finished);
    CHECK(content(b) == "pre");
}

TEST_CASE("MTEXT: columns and background mask chosen in the text box") {
    Fixture f;
    f.host.run("MTEXT", {"H", "2", "0,100", "100,50"});  // now waiting for the text
    f.host.setSetting("tmp.mtext.columns", "2");
    f.host.setSetting("tmp.mtext.colkind", "static");
    f.host.setSetting("tmp.mtext.bgmask", "1");
    f.host.feed({"a\\Pb\\Pc"});
    const MTextData* m = f.firstMText();
    REQUIRE(m);
    CHECK(m->columns == 2);
    CHECK(m->width == doctest::Approx((100 - 4) / 2.0));
    CHECK(m->columnHeight == doctest::Approx(50));  // the rectangle's height
    CHECK(m->bgMode == 1);

    Fixture g;
    g.host.run("MTEXT", {"C", "3", "0,100", "100,50"});
    g.host.setSetting("tmp.mtext.columns", "1");  // "No columns" in the box wins
    g.host.feed({"x"});
    REQUIRE(g.firstMText());
    CHECK(g.firstMText()->columns == 1);
    CHECK(g.firstMText()->width == doctest::Approx(100));
    CHECK(g.firstMText()->bgMode == 0);
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

TEST_CASE("TXT2MTXT SEttings: selection order and word wrap into one paragraph") {
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
    EntityId low = addText({0, 0}, "second");
    EntityId high = addText({0, 10}, "first line");
    REQUIRE(f.host
                .run("TXT2MTXT", {"SEttings", "Order", "Select", "Wrap", "Yes", "eXit",
                                  cmd::ScriptItem::select({low, high}), cmd::ScriptItem::enter()})
                .finished);
    const MTextData* m = f.firstMText();
    REQUIRE(m);
    CHECK(m->utf8 == "second first line");  // as selected, one paragraph
    CHECK(m->width > 0);
    CHECK(m->position.y == doctest::Approx(12.5));  // still starts at the top line
    // the settings are remembered
    CHECK(f.host.setting("txt2mtxt.wrap") == std::optional<std::string>("1"));
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

TEST_CASE("SPELL suggests the nearest word and does not read MTEXT codes as words") {
    Fixture f;
    std::vector<EntityId> ids;
    for (const char* s :
         {"Kichen \\Lwall\\l and teh floor", "{\\fArial|b1;thikness} \\S1/2;\\Pdoor"}) {
        MTextData m;
        m.utf8 = s;
        Transaction tx = f.doc.begin("setup");
        Entity e;
        e.data = Indirect<MTextData>(std::move(m));
        ids.push_back(tx.add(std::move(e)).value());
        tx.commit();
    }
    // Three typos in all: Kichen, teh, thikness. Codes (\L \l \f..; \S..; \P) are never asked
    // about.
    REQUIRE(f.host
                .run("SPELL", {cmd::ScriptItem::select(ids), cmd::ScriptItem::enter(), "Suggest",
                               "Suggest", "Suggest"})
                .finished);
    // Words are replaced from the end of each text, so the typos may be asked in either order, but
    // each text ends up corrected and keeps its markup.
    const std::string a = entityAs<MTextData>(*f.doc.find(ids[0]))->utf8;
    CHECK(a == "Kitchen \\Lwall\\l and the floor");
    CHECK(entityAs<MTextData>(*f.doc.find(ids[1]))->utf8 ==
          "{\\fArial|b1;thickness} \\S1/2;\\Pdoor");
}

TEST_CASE("MTEXTFORMAT sets the background mask and the line spacing") {
    Fixture f;
    MTextData m;
    m.utf8 = "note";
    Transaction tx = f.doc.begin("setup");
    Entity e;
    e.data = Indirect<MTextData>(std::move(m));
    const EntityId id = tx.add(std::move(e)).value();
    tx.commit();
    REQUIRE(f.host
                .run("MTEXTFORMAT", {cmd::ScriptItem::select({id}), cmd::ScriptItem::enter(),
                                     "Background", "Colour", "3", "2"})
                .finished);
    auto* d = entityAs<MTextData>(*f.doc.find(id));
    CHECK(d->bgMode == 2);
    CHECK(d->bgColor == 3);
    CHECK(d->bgScale == doctest::Approx(2));
    REQUIRE(f.host
                .run("MTEXTFORMAT", {cmd::ScriptItem::select({id}), cmd::ScriptItem::enter(),
                                     "Background", "Window", "9"})
                .finished);
    d = entityAs<MTextData>(*f.doc.find(id));
    CHECK(d->bgMode == 1);
    CHECK(d->bgScale == doctest::Approx(5));  // clamped
    REQUIRE(f.host
                .run("MTEXTFORMAT", {cmd::ScriptItem::select({id}), cmd::ScriptItem::enter(),
                                     "LineSpacing", "1.5"})
                .finished);
    CHECK(entityAs<MTextData>(*f.doc.find(id))->lineSpacing == doctest::Approx(1.5));
    REQUIRE(f.host
                .run("MTEXTFORMAT", {cmd::ScriptItem::select({id}), cmd::ScriptItem::enter(),
                                     "Background", "None"})
                .finished);
    CHECK(entityAs<MTextData>(*f.doc.find(id))->bgMode == 0);
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

TEST_CASE("TEXTALIGN along a line: Distribute spreads, Spacing sets the gap, Points slides") {
    Fixture f;
    f.host.run("TEXT", {"0,0", "2.5", "0", "A"});
    f.host.run("TEXT", {"7,30", "2.5", "0", "B"});
    f.host.run("TEXT", {"3,12", "2.5", "0", "C"});
    REQUIRE(f.doc.modelSpace().size() == 3);
    const EntityId ta = f.doc.modelSpace()[0];
    const EntityId tb = f.doc.modelSpace()[1];
    const EntityId tc = f.doc.modelSpace()[2];
    auto corner = [&](EntityId id) { return geom::bounds(*f.doc.find(id), f.doc).min; };
    const Vec2 a = corner(ta);
    // a vertical line up from A: C (lower) first, then B, evenly up to y + 40
    REQUIRE(
        f.host
            .run("TEXTALIGN", {cmd::ScriptItem::select({ta, tb, tc}), cmd::ScriptItem::enter(),
                               "Distribute", std::to_string(a.x) + "," + std::to_string(a.y + 40)})
            .finished);
    CHECK(corner(tc).x == doctest::Approx(a.x));
    CHECK(corner(tc).y == doctest::Approx(a.y + 20));
    CHECK(corner(tb).y == doctest::Approx(a.y + 40));
    CHECK(f.host.undo());
    REQUIRE(f.host
                .run("TEXTALIGN",
                     {cmd::ScriptItem::select({ta, tb, tc}), cmd::ScriptItem::enter(), "Spacing",
                      "5", std::to_string(a.x) + "," + std::to_string(a.y + 40)})
                .finished);
    CHECK(corner(tc).y == doctest::Approx(a.y + 5));
    CHECK(corner(tb).y == doctest::Approx(a.y + 10));
    CHECK(f.host.undo());
    const double bY = corner(tb).y;
    REQUIRE(f.host
                .run("TEXTALIGN", {cmd::ScriptItem::select({ta, tb, tc}), cmd::ScriptItem::enter(),
                                   "Points", std::to_string(a.x) + "," + std::to_string(a.y + 40)})
                .finished);
    CHECK(corner(tb).x == doctest::Approx(a.x));  // onto the line
    CHECK(corner(tb).y == doctest::Approx(bY));   // at the same height
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

TEST_CASE("MTEXT: the Columns option cuts the rectangle into columns") {
    Fixture f;
    f.host.run("MTEXT", {"C", "3", "H", "2", "0,100", "100,50", "a\\Pb\\Pc"});
    const MTextData* m = f.firstMText();
    REQUIRE(m);
    CHECK(m->columns == 3);
    CHECK(m->columnGap == doctest::Approx(4));  // two text heights
    CHECK(m->width == doctest::Approx((100 - 2 * 4) / 3.0));
}

TEST_CASE("MTEXT: the Line spacing option sets the line pitch factor") {
    Fixture f;
    f.host.run("MTEXT", {"L", "E", "1.5", "H", "2", "0,100", "100,50", "a\\Pb"});
    const MTextData* m = f.firstMText();
    REQUIRE(m);
    CHECK(m->lineSpacing == doctest::Approx(1.5));
}

TEST_CASE("EXPLODE turns multiline text into one single-line text per line - undo restores it") {
    Fixture f;
    Transaction tx = f.doc.begin("add");
    MTextData m;
    m.position = {10, 100};
    m.height = 4;
    m.width = 0;
    m.rotation = kPi / 6;
    m.utf8 = "First line\\PSecond\\P\\PFourth";
    Entity e;
    e.data = Indirect<MTextData>(m);
    auto id = tx.add(std::move(e));
    REQUIRE(id.ok());
    tx.commit();
    f.host.setSelection({});
    f.host.run("EXPLODE", {cmd::ScriptItem::select({id.value()}), cmd::ScriptItem::enter()});
    int texts = 0, mtexts = 0;
    std::vector<std::string> lines;
    f.doc.forEachEntity([&](const Entity& x) {
        if (const auto* t = entityAs<TextData>(x)) {
            ++texts;
            lines.push_back(t->utf8);
            CHECK(t->rotation == doctest::Approx(kPi / 6));
            CHECK(t->height == doctest::Approx(4).epsilon(0.3));
        }
        if (entityAs<MTextData>(x))
            ++mtexts;
    });
    CHECK(mtexts == 0);
    REQUIRE(texts == 3);  // the empty paragraph makes no text
    CHECK(lines[0] == "First line");
    CHECK(f.host.undo());
    mtexts = texts = 0;
    f.doc.forEachEntity([&](const Entity& x) {
        texts += entityAs<TextData>(x) != nullptr;
        mtexts += entityAs<MTextData>(x) != nullptr;
    });
    CHECK(mtexts == 1);
    CHECK(texts == 0);
}

TEST_CASE("EXPLODE of empty multiline text changes nothing and says so") {
    Fixture f;
    Transaction tx = f.doc.begin("add");
    MTextData m;
    m.position = {0, 0};
    m.utf8 = "\\P\\P";
    Entity e;
    e.data = Indirect<MTextData>(m);
    auto id = tx.add(std::move(e));
    REQUIRE(id.ok());
    tx.commit();
    f.host.setSelection({});
    f.host.run("EXPLODE", {cmd::ScriptItem::select({id.value()}), cmd::ScriptItem::enter()});
    int mtexts = 0, texts = 0;
    f.doc.forEachEntity([&](const Entity& x) {
        mtexts += entityAs<MTextData>(x) != nullptr;
        texts += entityAs<TextData>(x) != nullptr;
    });
    CHECK(mtexts == 1);
    CHECK(texts == 0);
    CHECK(f.host.logContains("empty"));
}

TEST_CASE("TEXTFIT stretches single-line text between two points and undoes") {
    Fixture f;
    f.host.run("TEXT", {"0,0", "2.5", "0", "ABC"});
    const EntityId id = f.doc.modelSpace()[0];
    REQUIRE(f.host
                .run("TEXTFIT",
                     {cmd::ScriptItem::select({id}), cmd::ScriptItem::enter(), "10,10", "10,60"})
                .finished);
    const TextData* t = entityAs<TextData>(*f.doc.find(id));
    CHECK(t->h == HAlign::Fit);
    CHECK(t->position.x == doctest::Approx(10));
    CHECK(t->alignPoint.y == doctest::Approx(60));
    CHECK(t->rotation == doctest::Approx(1.5707963267948966));
    f.host.undo();
    CHECK(entityAs<TextData>(*f.doc.find(id))->h != HAlign::Fit);
}
