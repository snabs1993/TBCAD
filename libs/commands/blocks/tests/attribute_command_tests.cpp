#include "support.h"

using namespace tbcad;
using namespace tbcad::block_cmds;
using namespace tbcad::block_cmds::test;

namespace {

// Defines a block "Tag" with a line and two attributes (NAME editable, KEY constant).
BlockId makeTagBlock(Fixture& f) {
    EntityId ln = f.addLine(0, 0, 10, 0);
    auto r1 = f.host->run("ATTDEF", {"", "name", "Name", "Anon", "0,3", "2", "0"});
    REQUIRE(r1.finished);
    auto r2 = f.host->run("ATTDEF", {"C", "", "key", "K1", "0,6", "", ""});
    REQUIRE(r2.finished);
    std::vector<EntityId> ids{ln};
    for (EntityId id : f.modelSpace())
        if (id != ln)
            ids.push_back(id);
    f.host->setSelection(ids);
    f.host->run("BLOCK", {"Tag", "0,0", "D"});
    REQUIRE(f.doc.blockByName("Tag"));
    return f.doc.blockByName("Tag")->id;
}

}  // namespace

TEST_CASE("ATTDEF creates an attribute definition text") {
    Fixture f;
    auto r =
        f.host->run("ATTDEF", {"I", "V", "", "part-no", "Part number", "Z1", "5,5", "3", "90"});
    CHECK(r.finished);
    REQUIRE(f.modelSpace().size() == 1);
    const auto* t = entityAs<TextData>(*f.doc.find(f.modelSpace()[0]));
    REQUIRE(t);
    REQUIRE(t->attdef);
    CHECK(t->attdef->tag == "PART-NO");
    CHECK(t->attdef->prompt == "Part number");
    CHECK(t->attdef->defaultValue == "Z1");
    CHECK(t->attdef->invisible);
    CHECK(t->attdef->verify);
    CHECK_FALSE(t->attdef->constant);
    CHECK(t->position == Vec2{5, 5});
    CHECK(t->height == doctest::Approx(3));
    CHECK(t->rotation == doctest::Approx(kPi / 2));
    CHECK(f.host->undo());
    CHECK(f.modelSpace().empty());
}

TEST_CASE("ATTDEF rejects a tag with spaces and asks again") {
    Fixture f;
    f.host->clearLog();
    f.host->run("ATTDEF", {"", "two words", "ONE", "", "", "0,0", "", ""});
    CHECK(f.host->logContains("spaces"));
    REQUIRE(f.modelSpace().size() == 1);
    CHECK(entityAs<TextData>(*f.doc.find(f.modelSpace()[0]))->attdef->tag == "ONE");
}

TEST_CASE("BLOCK keeps attributes and rejects duplicate tags") {
    Fixture f;
    f.host->run("ATTDEF", {"", "A", "", "", "0,0", "", ""});
    f.host->run("ATTDEF", {"", "a", "", "", "0,1", "", ""});
    f.host->setSelection(f.modelSpace());
    f.host->clearLog();
    f.host->run("BLOCK", {"Dup", "0,0", "D"});
    CHECK(f.host->logContains("own tag"));
    CHECK(f.doc.blockByName("Dup") == nullptr);
    CHECK(f.modelSpace().size() == 2);
}

TEST_CASE("INSERT asks for attribute values - constant ones are not asked") {
    Fixture f;
    makeTagBlock(f);
    auto r = f.host->run("INSERT", {"Tag", "10,10", "Bob"});
    CHECK(r.finished);
    REQUIRE(f.modelSpace().size() == 1);
    const auto* ins = entityAs<InsertData>(*f.doc.find(f.modelSpace()[0]));
    REQUIRE(ins);
    REQUIRE(ins->attribs.size() == 1);
    CHECK(ins->attribs[0].tag == "NAME");
    CHECK(ins->attribs[0].value == "Bob");
}

TEST_CASE("INSERT keeps the default when Enter is pressed, and honours block.attreq=0") {
    Fixture f;
    makeTagBlock(f);
    f.host->run("INSERT", {"Tag", "0,0", ""});
    const auto* a = entityAs<InsertData>(*f.doc.find(f.modelSpace()[0]));
    REQUIRE(a->attribs.size() == 1);
    CHECK(a->attribs[0].value == "Anon");

    f.host->setSetting("block.attreq", "0");
    f.host->run("INSERT", {"Tag", "5,5"});
    REQUIRE(f.modelSpace().size() == 2);
    const auto* b = entityAs<InsertData>(*f.doc.find(f.modelSpace()[1]));
    CHECK(b->attribs[0].value == "Anon");
}

TEST_CASE("ATTEDIT changes the values of an insert and can be undone") {
    Fixture f;
    makeTagBlock(f);
    f.host->run("INSERT", {"Tag", "0,0", "Zed"});
    EntityId i = f.modelSpace().back();
    auto r = f.host->run("ATTEDIT", {cmd::ScriptItem::pick(i), "Alice"});
    CHECK(r.finished);
    const auto* ins = entityAs<InsertData>(*f.doc.find(i));
    REQUIRE(ins->attribs.size() == 1);
    CHECK(ins->attribs[0].value == "Alice");
    CHECK(f.host->undo());
    CHECK(entityAs<InsertData>(*f.doc.find(i))->attribs[0].value == "Zed");
}

TEST_CASE("redefining a block updates its inserts - ATTSYNC does the same on demand") {
    Fixture f;
    BlockId tag = makeTagBlock(f);
    f.host->run("INSERT", {"Tag", "0,0", "Zed"});
    EntityId i = f.modelSpace().back();
    // Add a second editable attribute to the block definition outside BLOCK.
    {
        TextData t;
        t.utf8 = "EXTRA";
        t.attdef = AttDefInfo{"EXTRA", "", "e0", false, false, false, false};
        Entity e;
        e.data = Indirect<TextData>(std::move(t));
        Transaction tx = f.doc.begin("edit block");
        REQUIRE(tx.add(std::move(e), tag));
        tx.commit();
    }
    CHECK(entityAs<InsertData>(*f.doc.find(i))->attribs.size() == 1);
    auto r = f.host->run("ATTSYNC", {"Tag"});
    CHECK(r.finished);
    const auto* ins = entityAs<InsertData>(*f.doc.find(i));
    REQUIRE(ins->attribs.size() == 2);
    CHECK(ins->attribs[0].value == "Zed");
    CHECK(ins->attribs[1].value == "e0");
}

namespace {

// A host with a window: ATTDEF opens the dialog instead of asking on the command line.
struct DialogHost : cmd::ScriptHost {
    using cmd::ScriptHost::ScriptHost;
    std::vector<std::string> opened;
    bool dialogsAvailable() const override { return true; }
    void openPanel(std::string_view id) override { opened.emplace_back(id); }
};

}  // namespace

TEST_CASE("ATTDEF opens the dialog in a window - the dialog's values leave only the start point") {
    Document doc;
    cmd::Registry reg;
    registerAllCommands(reg);
    DialogHost host(doc, reg);

    auto first = host.run("ATTDEF", {});
    CHECK(first.finished);
    REQUIRE(host.opened.size() == 1);
    CHECK(host.opened[0] == "attdef");
    CHECK(doc.modelSpace().empty());

    // What the dialog stores when OK is pressed.
    host.setSetting("tmp.attdef.tag", "PART");
    host.setSetting("tmp.attdef.prompt", "Part number");
    host.setSetting("tmp.attdef.default", "P-1");
    host.setSetting("tmp.attdef.invisible", "1");
    host.setSetting("attdef.height", "4");
    host.setSetting("tmp.attdef.rotation", "90");
    host.setSetting("tmp.attdef.go", "1");
    auto r = host.run("ATTDEF", {"5,5"});
    CHECK(r.finished);
    REQUIRE(doc.modelSpace().size() == 1);
    const Entity* e = doc.find(doc.modelSpace().front());
    const auto* t = entityAs<TextData>(*e);
    REQUIRE(t);
    CHECK(t->attdef->tag == "PART");
    CHECK(t->attdef->prompt == "Part number");
    CHECK(t->attdef->defaultValue == "P-1");
    CHECK(t->attdef->invisible);
    CHECK(t->height == doctest::Approx(4));
    CHECK(t->rotation == doctest::Approx(3.14159265358979 / 2));
    CHECK(host.setting("tmp.attdef.go") == std::optional<std::string>("0"));  // one use per dialog
}

TEST_CASE("-ATTDEF always asks on the command line") {
    Document doc;
    cmd::Registry reg;
    registerAllCommands(reg);
    DialogHost host(doc, reg);
    auto r = host.run("-ATTDEF", {"", "tag1", "Tag one", "x", "0,0", "2", "0"});
    CHECK(r.finished);
    CHECK(host.opened.empty());
    CHECK(doc.modelSpace().size() == 1);
}

TEST_CASE("INSERT in a window asks the attribute values in one dialog") {
    Fixture f;
    makeTagBlock(f);  // NAME (editable, default Anon) and a constant KEY
    DialogHost host(f.doc, f.reg);

    // The insert is placed, then the dialog is opened and INSERT waits for its OK.
    auto r = host.run("INSERT", {"Tag", "50,50"});
    CHECK_FALSE(r.finished);
    REQUIRE(host.opened.size() == 1);
    CHECK(host.opened[0] == "attvalues");
    const std::string fields = host.setting("tmp.insatt.fields").value_or("");
    CHECK(fields.find("NAME") != std::string::npos);
    CHECK(fields.find("KEY") == std::string::npos);  // constant: not asked

    // What the dialog does on OK: store the values, then press Enter.
    host.setSetting("tmp.insatt.result", std::string("NAME\x1f") + "Ada\x1e");
    r = host.feed({cmd::ScriptItem::enter()});
    CHECK(r.finished);
    bool found = false;
    f.doc.forEachEntity([&](const Entity& e) {
        if (const auto* ins = entityAs<InsertData>(e))
            for (const AttribValue& v : ins->attribs)
                if (v.tag == "NAME") {
                    found = true;
                    CHECK(v.value == "Ada");
                }
    });
    CHECK(found);
}
