#include <doctest/doctest.h>

#include "tbcad/cmd/cmd.h"
#include "tbcad/cmd/register_all.h"
#include "tbcad/symbolscmd/symbolscmd.h"

using namespace tbcad;

namespace {
struct Fixture {
    Document doc;
    cmd::Registry reg;
    cmd::ScriptHost host;
    Fixture() : host(doc, (registerAllCommands(reg), reg)) {}
    const InsertData* lastInsert() const {
        const InsertData* found = nullptr;
        EntityId best{};
        doc.forEachEntity([&](const Entity& e) {
            if (const auto* i = entityAs<InsertData>(e); i && e.common.id > best) {
                best = e.common.id;
                found = i;
            }
        });
        return found;
    }
    const Entity* lastInsertEntity() const {
        const Entity* found = nullptr;
        doc.forEachEntity([&](const Entity& e) {
            if (kind(e) == EntityKind::Insert && (!found || e.common.id > found->common.id))
                found = &e;
        });
        return found;
    }
};
}  // namespace

TEST_CASE("every symbol has geometry and the attributes it promises") {
    for (const auto& info : symbolscmd::symbolList()) {
        auto es = symbolscmd::symbolEntities(info.symbol);
        CHECK_MESSAGE(!es.empty(), info.command);
        for (const Entity& e : es)
            CHECK(e.common.layer.value == 0);  // layer 0: follows the layer of the insert
    }
    int attdefs = 0;
    for (const Entity& e : symbolscmd::symbolEntities(symbolscmd::Symbol::SectionMarker))
        if (const auto* t = entityAs<TextData>(e); t && t->attdef)
            ++attdefs;
    CHECK(attdefs == 2);
}

TEST_CASE("GRIDBUBBLE makes the block once and inserts it annotative at the current scale") {
    Fixture f;
    REQUIRE(f.host.run("CANNOSCALE", {"50"}).finished);
    REQUIRE(f.host.run("GRIDBUBBLE", {"100,200", "", "B"}).finished);
    const BlockDef* def = f.doc.blockByName("TB_GRID");
    REQUIRE(def);
    const Entity* e = f.lastInsertEntity();
    REQUIRE(e);
    const auto* ins = entityAs<InsertData>(*e);
    REQUIRE(ins);
    CHECK(ins->block == def->id);
    CHECK(ins->position.x == doctest::Approx(100));
    CHECK(ins->scale.x == doctest::Approx(50));
    REQUIRE(ins->attribs.size() == 1);
    CHECK(ins->attribs[0].tag == "ID");
    CHECK(ins->attribs[0].value == "B");
    CHECK(e->common.annotative);
    REQUIRE(e->common.annoScales.size() == 1);
    CHECK(e->common.annoScales.front() == doctest::Approx(50));

    // The second one reuses the block; Enter keeps the default letter.
    REQUIRE(f.host.run("GB", {"150,200", "", ""}).finished);
    std::size_t blocks = 0;
    for (const auto& [id, b] : f.doc.blocks())
        if (b.name == "TB_GRID")
            ++blocks;
    CHECK(blocks == 1);
    CHECK(f.lastInsert()->attribs[0].value == "A");
}

TEST_CASE("all symbols can be placed with default attributes - one undo step each") {
    Fixture f;
    for (const auto& info : symbolscmd::symbolList()) {
        std::vector<cmd::ScriptItem> items = {"0,0", ""};
        for (int i = 0; i < 3; ++i)
            items.push_back("");  // accept the defaults of up to three attributes
        auto r = f.host.run(info.command, items);
        CHECK_MESSAGE(r.finished, info.command);
        CHECK_MESSAGE(f.doc.blockByName(info.blockName) != nullptr, info.command);
    }
    f.host.undo();
    CHECK(f.doc.blockByName("TB_DETAIL") == nullptr);  // the block went with the undone step
}
