#include "support.h"

using namespace tbcad;
using namespace tbcad::block_cmds;
using namespace tbcad::block_cmds::test;

namespace {

// A 10 x 5 rectangle of four lines as block "Frame", inserted at (100,0).
struct FrameFixture : Fixture {
    BlockId block{};
    EntityId insert{};
    FrameFixture() {
        Transaction tx = doc.begin("frame");
        block = tx.addBlock(BlockDef{.name = "Frame"}).value();
        auto put = [&](double a, double b, double c, double d) {
            Entity e;
            e.data = LineData{{a, b}, {c, d}};
            REQUIRE(tx.add(std::move(e), block).ok());
        };
        put(0, 0, 10, 0);
        put(10, 0, 10, 5);
        put(10, 5, 0, 5);
        put(0, 5, 0, 0);
        tx.commit();
        Entity e;
        e.data = Indirect<InsertData>(InsertData{block, {100, 0}, {1, 1}, 0});
        insert = add(std::move(e));
    }
    const BlockDef& def() const { return *doc.block(block); }
    // Working copies (lines in model space), and the one from `a` to `b`.
    std::vector<EntityId> copies() const {
        std::vector<EntityId> out;
        for (EntityId id : modelSpace())
            if (kind(*doc.find(id)) == EntityKind::Line)
                out.push_back(id);
        return out;
    }
    EntityId copy(Vec2 a, Vec2 b) const {
        for (EntityId id : copies()) {
            const auto* l = entityAs<LineData>(*doc.find(id));
            if (near(l->a, a, 1e-6) && near(l->b, b, 1e-6))
                return id;
        }
        return {};
    }
    bool inBlock(const std::vector<EntityId>& ids) const {
        for (EntityId id : ids)
            if (std::find(def().entities.begin(), def().entities.end(), id) == def().entities.end())
                return false;
        return true;
    }
};

}  // namespace

TEST_CASE("block editor: parameters and actions still name the block's objects after BSAVE") {
    FrameFixture f;
    f.host->run("BPARAM", {"Frame", "L", "Width", "0,0", "10,0", "0", "0"});
    f.host->run("BACTION", {"Frame", "Width", "St", "9,-1", "11,6", "-1,-1", "11,6"});
    REQUIRE(f.def().dynamic->actions.size() == 1);
    f.host->run("BEDIT", {cmd::ScriptItem::pick(f.insert)});
    f.host->run("BSAVE", {});
    CHECK(f.def().dynamic->actions[0].objects.size() == 4);
    CHECK(f.inBlock(f.def().dynamic->actions[0].objects));
    // A line erased in the editor leaves the action.
    {
        Transaction tx = f.doc.begin("erase");
        REQUIRE(tx.erase(f.copy({100, 5}, {100, 0})).ok());
        tx.commit();
    }
    f.host->run("BCLOSE", {cmd::ScriptItem::enter()});
    CHECK(f.def().dynamic->actions[0].objects.size() == 3);
    CHECK(f.inBlock(f.def().dynamic->actions[0].objects));
    f.host->run("DYNSET", {cmd::ScriptItem::pick(f.insert), "Width", "14"});
    CHECK(near(blocks::insertBounds(f.doc, *f.doc.find(f.insert)).max, {114, 5}));
}

TEST_CASE("block editor: BPARAM and BACTION take drawing points and picked objects") {
    FrameFixture f;
    f.host->run("BEDIT", {cmd::ScriptItem::pick(f.insert)});
    REQUIRE(f.copies().size() == 4);
    // No block name inside the editor; the points are where the copies are.
    f.host->run("BPARAM", {"L", "Width", "100,0", "110,0", "0", "0"});
    REQUIRE(f.def().dynamic);
    const DynParam& p = f.def().dynamic->params[0];
    CHECK(near(p.base, {0, 0}));
    CHECK(near(p.end, {10, 0}));
    f.host->run("BACTION", {"Width", "St", "109,-1", "111,6", cmd::ScriptItem::select(f.copies()),
                            cmd::ScriptItem::enter()});
    REQUIRE(f.def().dynamic->actions.size() == 1);
    const DynAction& a = f.def().dynamic->actions[0];
    CHECK(near(a.frameMin, {9, -1}));
    CHECK(near(a.frameMax, {11, 6}));
    CHECK(a.objects.size() == 4);
    CHECK(f.inBlock(a.objects));
    f.host->run("BCLOSE", {cmd::ScriptItem::enter()});
    CHECK(f.copies().empty());
    f.host->run("DYNSET", {cmd::ScriptItem::pick(f.insert), "Width", "14"});
    CHECK(near(blocks::insertBounds(f.doc, *f.doc.find(f.insert)).max, {114, 5}));
}

TEST_CASE("block editor: visibility states with BVSTATE, BVHIDE and BVSHOW") {
    FrameFixture f;
    f.host->run("BEDIT", {cmd::ScriptItem::pick(f.insert)});
    f.host->run("BVSTATE", {"New", "Open", "Current"});
    REQUIRE(f.def().dynamic);
    const DynParam& p = f.def().dynamic->params[0];
    CHECK(p.kind == DynParam::Kind::Visibility);
    CHECK(p.states == std::vector<std::string>{"VisibilityState0", "Open"});
    const EntityId top = f.copy({110, 5}, {100, 5});
    REQUIRE(top);
    f.host->setSelection({});  // BEDIT selected the copies
    f.host->run("BVHIDE", {cmd::ScriptItem::select({top}), cmd::ScriptItem::enter()});
    CHECK_FALSE(f.doc.find(top)->common.visible);
    CHECK(f.def().dynamic->params[0].stateObjects[1].size() == 3);
    CHECK(f.def().dynamic->params[0].stateObjects[0].size() == 4);
    // Showing the first state brings the line back; Open hides it again.
    f.host->run("BVSTATE", {"Set", "VisibilityState0"});
    CHECK(f.doc.find(top)->common.visible);
    f.host->run("BVSTATE", {"Set", "Open"});
    CHECK_FALSE(f.doc.find(top)->common.visible);
    f.host->run("BVSHOW", {"All"});
    CHECK(f.doc.find(top)->common.visible);
    CHECK(f.def().dynamic->params[0].stateObjects[1].size() == 4);
    f.host->run("BVHIDE", {cmd::ScriptItem::select({top}), cmd::ScriptItem::enter()});
    f.host->run("BVSTATE", {"Rename", "Open", "Gap"});
    CHECK(f.def().dynamic->params[0].states[1] == "Gap");
    // The block keeps every line visible; the state only hides it in inserts.
    f.host->run("BCLOSE", {cmd::ScriptItem::enter()});
    REQUIRE(f.def().entities.size() == 4);
    for (EntityId id : f.def().entities)
        CHECK(f.doc.find(id)->common.visible);
    CHECK(f.inBlock(f.def().dynamic->params[0].stateObjects[1]));
    CHECK(f.def().dynamic->params[0].stateObjects[1].size() == 3);
    f.host->run("DYNSET", {cmd::ScriptItem::pick(f.insert), "Visibility", "Gap"});
    CHECK(entityAs<InsertData>(*f.doc.find(f.insert))->dynValues == std::vector<double>{1});
}

TEST_CASE("block editor: BSAVEAS makes a new block and leaves the old one alone") {
    FrameFixture f;
    f.host->run("BPARAM", {"Frame", "L", "Width", "0,0", "10,0", "0", "0"});
    f.host->run("BACTION", {"Frame", "Width", "M", "-1,-1", "11,6"});
    f.host->run("BEDIT", {cmd::ScriptItem::pick(f.insert)});
    f.host->run("BSAVEAS", {"Frame2"});
    const BlockDef* two = f.doc.blockByName("Frame2");
    REQUIRE(two);
    CHECK(two->entities.size() == 4);
    REQUIRE(two->dynamic);
    const std::vector<EntityId> objs = two->dynamic->actions[0].objects;
    CHECK(objs.size() == 4);
    for (EntityId id : objs)
        CHECK(std::find(two->entities.begin(), two->entities.end(), id) != two->entities.end());
    // The edit goes on with Frame2.
    {
        Transaction tx = f.doc.begin("edit");
        REQUIRE(tx.erase(f.copy({100, 5}, {100, 0})).ok());
        tx.commit();
    }
    f.host->run("BCLOSE", {cmd::ScriptItem::enter()});
    CHECK(f.doc.blockByName("Frame2")->entities.size() == 3);
    CHECK(f.def().entities.size() == 4);
    CHECK(f.inBlock(f.def().dynamic->actions[0].objects));
    f.host->run("BEDIT", {"Name", "Frame"});
    f.host->run("BSAVEAS", {"Frame2"});  // taken
    CHECK(f.host->logContains("already exists"));
}

TEST_CASE("block editor: BTESTBLOCK places a test reference that closing removes") {
    FrameFixture f;
    f.host->run("BEDIT", {cmd::ScriptItem::pick(f.insert)});
    const std::size_t before = f.modelSpace().size();
    f.host->run("BTESTBLOCK", {"300,0"});
    REQUIRE(f.modelSpace().size() == before + 1);
    f.host->run("BSAVE", {});
    CHECK(f.def().entities.size() == 4);  // the test reference is not part of the block
    f.host->run("BTESTBLOCK", {"Close"});
    CHECK(f.modelSpace().size() == before);
    f.host->run("BTESTBLOCK", {"Insert", "300,0"});
    f.host->run("BCLOSE", {cmd::ScriptItem::enter()});
    CHECK(f.modelSpace().size() == 1);  // just the original insert
    CHECK(f.def().entities.size() == 4);
}

TEST_CASE("BLOOKUPTABLE adds, changes and deletes rows of a lookup table") {
    FrameFixture f;
    f.host->run("BPARAM", {"Frame", "L", "Width", "0,0", "10,0", "0", "0"});
    f.host->run("BPARAM",
                {"Frame", "T", "Size", "0,8", "Width", "", "Small", "6", "Big", "12", ""});
    f.host->run("BLOOKUPTABLE",
                {"Frame", "Add", "Huge", "20", "Value", "Small", "7", "Delete", "Big", "List", ""});
    const DynParam& p = f.def().dynamic->params[1];
    CHECK(p.states == std::vector<std::string>{"Small", "Huge"});
    CHECK(p.lookupValues == std::vector<std::vector<double>>{{7}, {20}});
    CHECK(f.host->logContains("Huge | 20"));
    f.host->run("BLOOKUPTABLE", {"Frame", "Delete", "Small", ""});
    CHECK(f.host->logContains("at least two rows"));
}

TEST_CASE("BATTORDER changes the order of a block's attributes, also in the editor") {
    Fixture f;
    Transaction tx = f.doc.begin("blk");
    const BlockId b = tx.addBlock(BlockDef{.name = "Tag"}).value();
    for (const char* tag : {"A", "B", "C"}) {
        TextData t;
        t.utf8 = tag;
        t.height = 2.5;
        t.attdef.emplace(AttDefInfo{.tag = tag});
        REQUIRE(tx.add(Entity{{}, std::move(t)}, b).ok());
    }
    Entity line;
    line.data = LineData{{0, 0}, {5, 0}};
    REQUIRE(tx.add(std::move(line), b).ok());
    tx.commit();
    auto tags = [&] {
        std::string s;
        for (EntityId id : f.doc.block(b)->entities)
            if (const auto* t = entityAs<TextData>(*f.doc.find(id)); t && t->attdef)
                s += t->attdef->tag;
        return s;
    };
    f.host->run("BATTORDER", {"Tag", "c,a"});
    CHECK(tags() == "CAB");
    CHECK(f.host->logContains("Attribute order changed"));
    f.host->run("BATTORDER", {"Tag", "X"});
    CHECK(f.host->logContains("no attribute tagged \"X\""));

    f.host->run("BEDIT", {"Name", "Tag"});
    f.host->run("BATTORDER", {"B,C,A"});
    f.host->run("BCLOSE", {cmd::ScriptItem::enter()});
    CHECK(tags() == "BCA");
}
