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

TEST_CASE("expanding a big selection against a big group is linear") {
    Document doc;
    std::vector<EntityId> ids;
    {
        Transaction tx = doc.begin("gen");
        for (int i = 0; i < 100000; ++i) {
            Entity e;
            e.data = LineData{{double(i), 0}, {i + 1.0, 0}};
            ids.push_back(tx.add(std::move(e)).value());
        }
        tx.commit();
    }
    std::vector<EntityId> half(ids.begin(), ids.begin() + 50000),
        rest(ids.begin() + 50000, ids.end());
    {
        Transaction tx = doc.begin("group");
        REQUIRE(writeGroups(tx, {Group{"A", half, true}, Group{"B", rest, true}}).ok());
        tx.commit();
    }
    // One member of each group brings both whole groups; a selection of everything adds nothing.
    auto both = expandGroups(doc, std::vector<EntityId>{ids[3], ids[99999]});
    CHECK(both.size() == 100000);
    auto all = expandGroups(doc, ids);
    CHECK(all.size() == 100000);
    auto one = expandGroups(doc, std::vector<EntityId>{ids[7]});
    CHECK(one.size() == 50000);
    CHECK(one.front() == ids[7]);
}
