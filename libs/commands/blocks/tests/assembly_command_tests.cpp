#include "support.h"

using namespace tbcad;
using namespace tbcad::block_cmds;
using namespace tbcad::block_cmds::test;

namespace {

BlockId makeBar(Fixture& f) {
    Transaction tx = f.doc.begin("bar");
    BlockId b = tx.addBlock(BlockDef{.name = "Bar"}).value();
    Entity e;
    e.data = LineData{{0, 0}, {10, 0}};
    REQUIRE(tx.add(std::move(e), b).ok());
    tx.commit();
    return b;
}

EntityId insertBar(Fixture& f, BlockId b) {
    Entity e;
    e.data = Indirect<InsertData>(InsertData{b, {0, 0}, {1, 1}, 0});
    return f.add(std::move(e));
}

}  // namespace

TEST_CASE("ASMDEF defines parameters and presets, ASMSET and ASMINFO use them") {
    Fixture f;
    BlockId b = makeBar(f);
    // block, option, name, type, default, minimum, maximum
    f.host->run("ASMDEF", {"Bar", "Parameter", "Len", "Number", "10", "1", "100"});
    f.host->run("ASMDEF", {"Bar", "Parameter", "Mat", "Choice", "Oak", "Pine", ""});
    const BlockDef* def = f.doc.block(b);
    REQUIRE((def && def->assembly));
    REQUIRE(def->assembly->params.size() == 2);
    CHECK(def->assembly->params[0].max == doctest::Approx(100));
    CHECK(def->assembly->params[1].choices.size() == 2);
    // duplicate names are refused
    f.host->run("ASMDEF", {"Bar", "Parameter", "len", "Number", "1", "0", "0"});
    CHECK(f.doc.block(b)->assembly->params.size() == 2);

    // a bad formula is refused, a good one is saved
    f.host->run("ASMDEF", {"Bar", "Bind", "Attribute", "TAG", "Len +"});
    CHECK(f.doc.block(b)->assembly->bindings.empty());
    f.host->run("ASMDEF", {"Bar", "Bind", "Attribute", "TAG", "\"Bar \" & Len"});
    CHECK(f.doc.block(b)->assembly->bindings.size() == 1);

    f.host->run("ASMDEF", {"Bar", "Preset", "long", "80", "Pine"});
    REQUIRE(f.doc.block(b)->assembly->presets.size() == 1);
    CHECK(f.doc.block(b)->assembly->presets[0].values[0].number == doctest::Approx(80));

    EntityId ins = insertBar(f, b);
    f.host->run("ASMSET", {cmd::ScriptItem::pick(ins), "Len", "30"});
    const auto* data = entityAs<InsertData>(*f.doc.find(ins));
    REQUIRE(data);
    auto v = blocks::asmEffectiveValues(*f.doc.block(b)->assembly, *data);
    CHECK(v[0].number == doctest::Approx(30));

    // outside the limits changes nothing
    f.host->run("ASMSET", {cmd::ScriptItem::pick(ins), "Len", "500"});
    CHECK(blocks::asmEffectiveValues(*f.doc.block(b)->assembly,
                                     *entityAs<InsertData>(*f.doc.find(ins)))[0]
              .number == doctest::Approx(30));

    f.host->run("ASMSET", {cmd::ScriptItem::pick(ins), "Preset", "long"});
    v = blocks::asmEffectiveValues(*f.doc.block(b)->assembly,
                                   *entityAs<InsertData>(*f.doc.find(ins)));
    CHECK(v[0].number == doctest::Approx(80));
    CHECK(v[1].text == "Pine");
    CHECK(f.host->undo());

    auto r = f.host->run("ASMINFO", {cmd::ScriptItem::pick(ins)});
    CHECK(r.finished);
}

TEST_CASE("-ASMSET changes the selected assemblies in one step") {
    Fixture f;
    BlockId b = makeBar(f);
    f.host->run("ASMDEF", {"Bar", "Parameter", "Len", "Number", "10", "1", "100"});
    f.host->run("ASMDEF", {"Bar", "Preset", "long", "80"});
    EntityId a = insertBar(f, b);
    EntityId c = insertBar(f, b);
    f.host->setSelection({a, c});
    f.host->run("-ASMSET", {"Len", "2 * 12", ""});
    for (EntityId id : {a, c})
        CHECK(blocks::asmEffectiveValues(*f.doc.block(b)->assembly,
                                         *entityAs<InsertData>(*f.doc.find(id)))[0]
                  .number == doctest::Approx(24));
    f.host->setSelection({a, c});
    f.host->run("-ASMSET", {"@preset", "long", ""});
    CHECK(blocks::asmEffectiveValues(*f.doc.block(b)->assembly,
                                     *entityAs<InsertData>(*f.doc.find(c)))[0]
              .number == doctest::Approx(80));
    CHECK(f.host->undo());
    CHECK(blocks::asmEffectiveValues(*f.doc.block(b)->assembly,
                                     *entityAs<InsertData>(*f.doc.find(a)))[0]
              .number == doctest::Approx(24));
}
