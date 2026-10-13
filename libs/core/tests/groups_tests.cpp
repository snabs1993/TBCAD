#include <doctest/doctest.h>

#include "tbcad/core/core.h"

using namespace tbcad;

namespace {
EntityId addLine(Document& doc, double x) {
    Transaction tx = doc.begin("add");
    Entity e;
    e.data = LineData{{x, 0}, {x + 1, 0}};
    auto id = tx.add(std::move(e));
    REQUIRE(id.ok());
    tx.commit();
    return id.value();
}
}  // namespace

TEST_CASE("groups round trip through the header and expand a pick to the whole group") {
    Document doc;
    EntityId a = addLine(doc, 0), b = addLine(doc, 2), c = addLine(doc, 4);
    CHECK(readGroups(doc).empty());
    CHECK(expandGroups(doc, std::vector<EntityId>{a}).size() == 1);
    {
        Transaction tx = doc.begin("group");
        REQUIRE(writeGroups(tx, {Group{"pair", {a, b}, true}}).ok());
        tx.commit();
    }
    auto groups = readGroups(doc);
    REQUIRE(groups.size() == 1);
    CHECK(groups[0].name == "pair");
    CHECK(groups[0].members.size() == 2);
    auto ex = expandGroups(doc, std::vector<EntityId>{b});
    CHECK(ex == std::vector<EntityId>{b, a});
    CHECK(expandGroups(doc, std::vector<EntityId>{c}) == std::vector<EntityId>{c});
}

TEST_CASE("erased members drop out and non-selectable groups do not expand") {
    Document doc;
    EntityId a = addLine(doc, 0), b = addLine(doc, 2);
    {
        Transaction tx = doc.begin("group");
        REQUIRE(writeGroups(tx, {Group{"g", {a, b}, false}}).ok());
        tx.commit();
    }
    CHECK(expandGroups(doc, std::vector<EntityId>{a}).size() == 1);
    {
        Transaction tx = doc.begin("erase");
        REQUIRE(tx.erase(b).ok());
        tx.commit();
    }
    auto groups = readGroups(doc);
    REQUIRE(groups.size() == 1);
    CHECK(groups[0].members == std::vector<EntityId>{a});
    {
        Transaction tx = doc.begin("erase");
        REQUIRE(tx.erase(a).ok());
        tx.commit();
    }
    CHECK(readGroups(doc).empty());
}
