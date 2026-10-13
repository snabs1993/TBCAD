#include "support.h"

using namespace tbcad;
using namespace tbcad::blocks;
using namespace tbcad::blocks::test;

namespace {

Entity attdef(const char* tag, const char* def, Vec2 pos, bool invisible = false,
              bool constant = false) {
    TextData t;
    t.position = pos;
    t.utf8 = tag;
    t.attdef = AttDefInfo{tag, "", def, invisible, constant, false, false};
    Entity e;
    e.data = Indirect<TextData>(std::move(t));
    return e;
}

// A stand-in text generator: one segment of "one unit per character" starting at the position.
void registerFakeText() {
    auto lineFor = [](const Entity& e) {
        const auto* t = entityAs<TextData>(e);
        return std::pair<Vec2, Vec2>{
            t->position, {t->position.x + static_cast<double>(t->utf8.size()), t->position.y}};
    };
    geom::DisplayRegistry::set(
        EntityKind::Text,
        [lineFor](const Document&, const Entity& e, const geom::DisplayContext&, DisplayList& out) {
            auto [a, b] = lineFor(e);
            DisplayPolyline p;
            p.pts = {a, b};
            out.items.push_back(std::move(p));
        },
        [lineFor](const Document&, const Entity& e) {
            auto [a, b] = lineFor(e);
            BBox2 box;
            box.expand(a);
            box.expand(b);
            return box;
        });
}

Entity insertWith(BlockId b, Vec2 pos, std::vector<AttribValue> values) {
    Entity e = insertOf(b, pos);
    entityAs<InsertData>(e)->attribs = std::move(values);
    return e;
}

}  // namespace

TEST_CASE("tags are upper-cased and validated") {
    CHECK(normalizeTag("part_no") == "PART_NO");
    CHECK(normalizeTag("เลขที่a") == "เลขที่A");
    CHECK(validateTag("PART").ok());
    CHECK_FALSE(validateTag("").ok());
    CHECK_FALSE(validateTag("TWO WORDS").ok());
    CHECK_FALSE(validateTag(std::string(256, 'X')).ok());
}

TEST_CASE("definitions, defaults, duplicates") {
    Document doc;
    BlockId b = addBlock(doc, "B",
                         {line(0, 0, 1, 0), attdef("A", "a0", {0, 1}),
                          attdef("K", "k0", {0, 2}, false, true), attdef("C", "c0", {0, 3})});
    auto defs = attributeDefinitions(doc, b);
    REQUIRE(defs.size() == 3);
    CHECK(hasEditableAttributes(doc, b));
    CHECK(duplicateTag(defs).empty());
    auto values = defaultAttribs(doc, b);
    REQUIRE(values.size() == 2);  // the constant one has no per-insert value
    CHECK(values[0].tag == "A");
    CHECK(values[1].value == "c0");

    BlockId d = addBlock(doc, "D", {attdef("X", "", {0, 0}), attdef("X", "", {0, 1})});
    CHECK(duplicateTag(attributeDefinitions(doc, d)) == "X");
    BlockId plain = addBlock(doc, "P", {line(0, 0, 1, 0)});
    CHECK_FALSE(hasEditableAttributes(doc, plain));
}

TEST_CASE("attributeText substitutes the value and honours invisible") {
    Document doc;
    BlockId b = addBlock(doc, "B", {attdef("A", "a0", {0, 1}), attdef("H", "hid", {0, 2}, true)});
    auto defs = attributeDefinitions(doc, b);
    InsertData ins{b, {0, 0}, {1, 1}, 0};
    ins.attribs = {{"A", "hello"}};
    auto shown = attributeText(*defs[0], ins);
    REQUIRE(shown);
    const auto* t = entityAs<TextData>(*shown);
    CHECK(t->utf8 == "hello");
    CHECK_FALSE(t->attdef.has_value());
    CHECK_FALSE(attributeText(*defs[1], ins).has_value());
}

TEST_CASE("an insert displays its own attribute values, not the block's shared ones") {
    registerInsertDisplay();
    registerFakeText();
    Document doc;
    BlockId b = addBlock(
        doc, "B", {line(0, 0, 1, 0), attdef("A", "dd", {0, 5}), attdef("H", "hid", {0, 6}, true)});
    EntityId one = addEntity(doc, insertWith(b, {10, 0}, {{"A", "ab"}}));
    EntityId two = addEntity(doc, insertWith(b, {20, 0}, {{"A", "abcd"}}));
    geom::DisplayContext ctx;
    ctx.pixelSizeMm = 0.1;
    for (auto [id, width, x] : {std::tuple{one, 2.0, 10.0}, std::tuple{two, 4.0, 20.0}}) {
        DisplayList l;
        geom::buildDisplay(doc, *doc.find(id), ctx, l);
        REQUIRE(l.items.size() == 2);  // the line and the visible attribute only
        const auto& p = std::get<DisplayPolyline>(l.items[1]);
        CHECK(near(p.pts[0], {x, 5}));
        CHECK(near(p.pts[1], {x + width, 5}));
    }
    BBox2 box = insertBounds(doc, *doc.find(two));
    CHECK(box.max.x == doctest::Approx(24));
    CHECK(box.max.y == doctest::Approx(5));
    geom::DisplayRegistry::clear(EntityKind::Text);
}

TEST_CASE("explode turns attributes into text with the insert's values") {
    Document doc;
    BlockId b =
        addBlock(doc, "B",
                 {line(0, 0, 1, 0), attdef("A", "dd", {0, 5}), attdef("H", "hid", {0, 6}, true),
                  attdef("K", "fixed", {0, 7}, false, true)});
    EntityId i = addEntity(doc, insertWith(b, {10, 0}, {{"A", "mine"}}));
    auto r = explode(doc, *doc.find(i));
    REQUIRE(r.ok());
    REQUIRE(r.value().entities.size() == 3);  // line, A, K; H is invisible
    const auto* a = entityAs<TextData>(r.value().entities[1]);
    REQUIRE(a);
    CHECK(a->utf8 == "mine");
    CHECK_FALSE(a->attdef.has_value());
    CHECK(near(a->position, {10, 5}));
    CHECK(entityAs<TextData>(r.value().entities[2])->utf8 == "fixed");
}

TEST_CASE("syncInserts follows a redefined block") {
    Document doc;
    BlockId b = addBlock(doc, "B", {attdef("A", "a0", {0, 1}), attdef("B2", "b0", {0, 2})});
    EntityId i = addEntity(doc, insertWith(b, {0, 0}, {{"A", "x"}, {"OLD", "gone"}}));
    CHECK(syncedAttribs(doc, *entityAs<InsertData>(*doc.find(i))).size() == 2);
    Transaction tx = doc.begin("sync");
    CHECK(syncInserts(tx, doc, b) == 1);
    tx.commit();
    const auto* ins = entityAs<InsertData>(*doc.find(i));
    REQUIRE(ins->attribs.size() == 2);
    CHECK(ins->attribs[0].value == "x");  // kept
    CHECK(ins->attribs[1].tag == "B2");   // new tag gets its default
    CHECK(ins->attribs[1].value == "b0");
    Transaction again = doc.begin("sync");
    CHECK(syncInserts(again, doc, b) == 0);
    again.rollback();
}
