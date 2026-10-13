#include "support.h"

using namespace tbcad;
using namespace tbcad::block_cmds;
using namespace tbcad::block_cmds::test;

namespace {

BlockId makeFrame(Fixture& f) {
    Transaction tx = f.doc.begin("frame");
    BlockId b = tx.addBlock(BlockDef{.name = "Frame"}).value();
    auto put = [&](Entity e) { REQUIRE(tx.add(std::move(e), b).ok()); };
    auto ln = [](double a, double b2, double c, double d) {
        Entity e;
        e.data = LineData{{a, b2}, {c, d}};
        return e;
    };
    put(ln(0, 0, 10, 0));
    put(ln(10, 0, 10, 5));
    put(ln(10, 5, 0, 5));
    put(ln(0, 5, 0, 0));
    tx.commit();
    return b;
}

EntityId insertFrame(Fixture& f, BlockId b) {
    Entity e;
    e.data = Indirect<InsertData>(InsertData{b, {100, 0}, {1, 1}, 0});
    return f.add(std::move(e));
}

}  // namespace

TEST_CASE("BPARAM and BACTION author a linear stretch that DYNSET drives") {
    Fixture f;
    BlockId b = makeFrame(f);
    // block, type, name, start, end, minimum, maximum
    f.host->run("BPARAM", {"Frame", "L", "Width", "0,0", "10,0", "0", "0"});
    const BlockDef* def = f.doc.block(b);
    REQUIRE((def && def->dynamic));
    REQUIRE(def->dynamic->params.size() == 1);
    CHECK(def->dynamic->params[0].name == "Width");
    CHECK(def->dynamic->params[0].kind == DynParam::Kind::Linear);

    // block, parameter, Stretch, frame corners, object corners
    f.host->run("BACTION", {"Frame", "Width", "St", "9,-1", "11,6", "-1,-1", "11,6"});
    def = f.doc.block(b);
    REQUIRE(def->dynamic->actions.size() == 1);
    CHECK(def->dynamic->actions[0].kind == DynAction::Kind::Stretch);
    CHECK(def->dynamic->actions[0].objects.size() == 4);

    EntityId ins = insertFrame(f, b);
    f.host->run("DYNSET", {cmd::ScriptItem::pick(ins), "Width", "14"});
    const auto* data = entityAs<InsertData>(*f.doc.find(ins));
    REQUIRE(data);
    REQUIRE(data->dynValues.size() == 1);
    CHECK(data->dynValues[0] == doctest::Approx(14));
    BBox2 box = blocks::insertBounds(f.doc, *f.doc.find(ins));
    CHECK(near(box.max, {114, 5}));
    CHECK(f.host->undo());
    CHECK(entityAs<InsertData>(*f.doc.find(ins))->dynValues.empty());
}

TEST_CASE("BPARAM refuses duplicates, unknown blocks and degenerate input") {
    Fixture f;
    BlockId b = makeFrame(f);
    f.host->run("BPARAM", {"Nope", "L", "W", "0,0", "1,0", "0", "0"});
    CHECK_FALSE(f.doc.block(b)->dynamic);
    f.host->run("BPARAM", {"Frame", "L", "W", "0,0", "0,0"});
    CHECK_FALSE(f.doc.block(b)->dynamic);
    f.host->run("BPARAM", {"Frame", "L", "W", "0,0", "10,0", "0", "0"});
    f.host->run("BPARAM", {"Frame", "L", "w", "0,0", "10,0", "0", "0"});
    REQUIRE(f.doc.block(b)->dynamic);
    CHECK(f.doc.block(b)->dynamic->params.size() == 1);
    f.host->run("BACTION", {"Frame", "W", "St", "9,-1", "11,6", "50,50", "60,60"});
    CHECK(f.doc.block(b)->dynamic->actions.empty());  // nothing inside the window
}

TEST_CASE("a visibility parameter takes its states from windows") {
    Fixture f;
    BlockId b = makeFrame(f);
    // states Open (bottom line) and Closed (top line), then Enter to finish
    f.host->run("BPARAM",
                {"Frame", "V", "Style", "Open", "-1,-1", "11,1", "Closed", "-1,4", "11,6", ""});
    const BlockDef* def = f.doc.block(b);
    REQUIRE(def->dynamic);
    const DynParam& p = def->dynamic->params[0];
    CHECK(p.kind == DynParam::Kind::Visibility);
    CHECK(p.states == std::vector<std::string>{"Open", "Closed"});
    CHECK(p.stateObjects[0].size() == 1);
    CHECK(p.stateObjects[1].size() == 1);
    EntityId ins = insertFrame(f, b);
    f.host->run("DYNSET", {cmd::ScriptItem::pick(ins), "Style", "Closed"});
    CHECK(entityAs<InsertData>(*f.doc.find(ins))->dynValues == std::vector<double>{1});
}
