#include "support.h"
#include "tbcad/text/text.h"

using namespace tbcad;
using namespace tbcad::dim;
using namespace tbcad::dim::test;

TEST_CASE("explode a linear dimension into lines, solid arrowheads and text") {
    Document doc;
    Entity e = entityOf(Indirect<DimensionData>(linear({0, 0}, {100, 0}, {50, 10})));
    e.common.color = Color::index(2);
    EntityId id = add(doc, std::move(e));
    auto r = explodeDimension(doc, *doc.find(id));
    REQUIRE(r.ok());
    int lines = 0, hatches = 0, texts = 0;
    for (const Entity& x : r.value()) {
        CHECK(x.common.id == EntityId{});
        CHECK(x.common.layer == doc.find(id)->common.layer);
        CHECK(x.common.color == Color::index(2));
        switch (kind(x)) {
            case EntityKind::Line:
                ++lines;
                break;
            case EntityKind::Hatch:
                ++hatches;
                CHECK(entityAs<HatchData>(x)->solid);
                break;
            case EntityKind::Text:
                ++texts;
                CHECK(entityAs<TextData>(x)->utf8 == "100.00");
                break;
            default:
                FAIL("unexpected kind");
        }
    }
    CHECK(lines == 3);
    CHECK(hatches == 2);
    CHECK(texts == 1);
}

TEST_CASE("explode keeps the text fill box and frame: a wipeout behind the text, then the frame") {
    Document doc;
    Transaction tx = doc.begin("style");
    REQUIRE(tx.modifyDimStyle(kDimStyleStandard, [](DimStyle& s) {
        s.textFill = 1;
        s.textFrame = true;
    }));
    tx.commit();
    EntityId id = add(doc, entityOf(Indirect<DimensionData>(linear({0, 0}, {100, 0}, {50, 10}))));
    auto r = explodeDimension(doc, *doc.find(id));
    REQUIRE(r.ok());
    int wipeouts = 0, frames = 0;
    std::size_t wipeAt = 0, frameAt = 0, textAt = 0;
    for (std::size_t i = 0; i < r.value().size(); ++i) {
        const Entity& x = r.value()[i];
        if (x.common.wipeout) {
            ++wipeouts;
            wipeAt = i;
        } else if (kind(x) == EntityKind::Polyline) {
            ++frames;
            frameAt = i;
        } else if (kind(x) == EntityKind::Text) {
            textAt = i;
        }
    }
    CHECK(wipeouts == 1);
    CHECK(frames == 1);
    CHECK(wipeAt < frameAt);
    CHECK(wipeAt < textAt);

    Transaction tx2 = doc.begin("colour");
    REQUIRE(tx2.modifyDimStyle(kDimStyleStandard, [](DimStyle& s) {
        s.textFill = 2;
        s.textFillColor = 3;
    }));
    tx2.commit();
    auto r2 = explodeDimension(doc, *doc.find(id));
    REQUIRE(r2.ok());
    int fills = 0;
    for (const Entity& x : r2.value())
        if (kind(x) == EntityKind::Hatch && x.common.color == Color::index(3))
            ++fills;
    CHECK(fills == 1);
}

TEST_CASE("explode an angular dimension keeps its arc as an arc - a leader gives MText") {
    Document doc;
    EntityId a = add(
        doc,
        entityOf(Indirect<DimensionData>(angular2({0, 0}, {10, 0}, {0, 0}, {0, 10}, {15, 15}))));
    auto r = explodeDimension(doc, *doc.find(a));
    REQUIRE(r.ok());
    int arcs = 0;
    for (const Entity& x : r.value())
        arcs += kind(x) == EntityKind::Arc ? 1 : 0;
    CHECK(arcs == 1);

    LeaderData ld;
    ld.vertices = {{0, 0}, {10, 10}};
    ld.textPos = {12, 10};
    ld.utf8 = "Note";
    EntityId l = add(doc, entityOf(Indirect<LeaderData>(ld)));
    auto lr = explodeDimension(doc, *doc.find(l));
    REQUIRE(lr.ok());
    int mtexts = 0;
    for (const Entity& x : lr.value())
        mtexts += kind(x) == EntityKind::MText ? 1 : 0;
    CHECK(mtexts == 1);

    Entity line;
    line.data = LineData{{0, 0}, {1, 1}};
    CHECK(!explodeDimension(doc, line).ok());
}

TEST_CASE("a multileader lays out its lines, arrowheads, dogleg and note, its block shows too") {
    Document doc;
    BlockId tag;
    {
        Transaction tx = doc.begin("block");
        BlockDef def;
        def.name = "Tag";
        tag = tx.addBlock(std::move(def)).value();
        REQUIRE(tx.add(entityOf(LineData{{0, 0}, {1, 0}}), tag));
        tx.commit();
    }
    MLeaderData m;
    m.lines = {{{0, 0}, {20, 10}}, {{0, 30}, {20, 10}}};
    m.landing = {20, 10};
    m.contentPos = {22.5, 10};
    m.utf8 = "Steel beam";
    Layout L = layoutMLeader(doc, m);
    CHECK(L.lines.size() == 3);  // two lines and the dogleg
    CHECK(L.arrows.size() == 2);
    REQUIRE(L.text.valid());
    CHECK(L.text.center.x == doctest::Approx(22.5 + 0.625));
    CHECK_FALSE(L.text.attachRight);

    m.contentPos = {17.5, 10};  // note on the left
    CHECK(layoutMLeader(doc, m).text.attachRight);

    m.contentPos = {22.5, 10};
    m.dogleg = false;
    CHECK(layoutMLeader(doc, m).lines.size() == 2);

    m.textHeight = 5;
    CHECK(layoutMLeader(doc, m).text.height == doctest::Approx(5));

    // As an object: explodes into lines, arrowheads and text; a block adds an insert.
    m.dogleg = true;
    m.textHeight = 0;
    EntityId id = add(doc, entityOf(Indirect<MLeaderData>(m)));
    auto parts = explodeDimension(doc, *doc.find(id));
    REQUIRE(parts.ok());
    int hatches = 0, texts = 0;
    for (const Entity& x : parts.value()) {
        hatches += kind(x) == EntityKind::Hatch;
        texts += kind(x) == EntityKind::MText;
    }
    CHECK(hatches == 2);
    CHECK(texts == 1);

    m.content = MLeaderData::Content::Block;
    m.block = tag;
    m.utf8.clear();
    EntityId withBlock = add(doc, entityOf(Indirect<MLeaderData>(m)));
    auto blockParts = explodeDimension(doc, *doc.find(withBlock));
    REQUIRE(blockParts.ok());
    int inserts = 0;
    for (const Entity& x : blockParts.value())
        inserts += kind(x) == EntityKind::Insert;
    CHECK(inserts == 1);

    // The older objects another program can read: leaders per line, a dogleg line, the note.
    auto old = mleaderAsLeaders(doc, *doc.find(id));
    REQUIRE(old.ok());
    int leaders = 0, lines = 0, mtexts = 0;
    for (const Entity& x : old.value()) {
        leaders += kind(x) == EntityKind::Leader;
        lines += kind(x) == EntityKind::Line;
        mtexts += kind(x) == EntityKind::MText;
    }
    CHECK(leaders == 2);
    CHECK(lines == 1);
    CHECK(mtexts == 1);
    CHECK_FALSE(mleaderAsLeaders(doc, entityOf(LineData{})).ok());
}

TEST_CASE("a deviation tolerance explodes into MTEXT with the stack, centred on the text spot") {
    Document doc;
    Transaction tx = doc.begin("style");
    REQUIRE(tx.modifyDimStyle(kDimStyleStandard, [](DimStyle& s) {
        s.tolerance = DimTolerance::Deviation;
        s.tolUpper = 0.1;
        s.tolLower = 0.05;
    }));
    tx.commit();
    EntityId id = add(doc, entityOf(Indirect<DimensionData>(linear({0, 0}, {100, 0}, {50, 10}))));
    const Layout L = layoutDimension(doc, *entityAs<DimensionData>(*doc.find(id)), 0.01);
    CHECK(L.text.stacked);
    // Wider than the measurement alone, narrower than the inline form.
    const TextStyle* ts = text::styleFor(doc, kTextStyleStandard);
    const double th = styleOf(doc, kDimStyleStandard).textHeight;
    CHECK(L.text.width > text::measureWidth(ts, "100.00 ", th));
    CHECK(L.text.width < text::measureWidth(ts, "100.00 +0.10 -0.05", th));
    // DIMTFAC makes the stack narrower.
    Transaction tx2 = doc.begin("tfac");
    REQUIRE(tx2.modifyDimStyle(kDimStyleStandard, [](DimStyle& s) { s.tolHeightFactor = 0.5; }));
    tx2.commit();
    const Layout small = layoutDimension(doc, *entityAs<DimensionData>(*doc.find(id)), 0.01);
    CHECK(small.text.width < L.text.width);
    Transaction tx3 = doc.begin("tfac back");
    REQUIRE(tx3.modifyDimStyle(kDimStyleStandard, [](DimStyle& s) { s.tolHeightFactor = 1; }));
    tx3.commit();
    auto r = explodeDimension(doc, *doc.find(id));
    REQUIRE(r.ok());
    int mtexts = 0;
    for (const Entity& x : r.value())
        if (const auto* m = entityAs<MTextData>(x)) {
            ++mtexts;
            CHECK(m->utf8 == "\\A1;100.00 {\\H1.42857x;\\S+0.10^-0.05;}");
            CHECK(m->attach == Attachment::MiddleCenter);
        } else {
            CHECK(kind(x) != EntityKind::Text);
        }
    CHECK(mtexts == 1);
}
