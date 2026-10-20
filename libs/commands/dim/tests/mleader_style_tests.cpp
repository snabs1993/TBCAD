#include <doctest/doctest.h>

#include "../src/dim_internal.h"
#include "tbcad/cmd/cmd.h"
#include "tbcad/cmd/script_host.h"
#include "tbcad/dim/dim.h"
#include "tbcad/dimcmd/dimcmd.h"

using namespace tbcad;
using namespace tbcad::cmd;

namespace {
struct Fixture {
    Document doc;
    Registry reg;
    std::unique_ptr<ScriptHost> host;
    Fixture() {
        registerBuiltinCommands(reg);
        dimcmd::registerCommands(reg);
        host = std::make_unique<ScriptHost>(doc, reg);
    }
    std::size_t leaders() const {
        std::size_t n = 0;
        doc.forEachEntity([&](const Entity& e) {
            if (entityAs<MLeaderData>(e))
                ++n;
        });
        return n;
    }
};
}  // namespace

TEST_CASE("MLEADERSTYLE New/Edit sets arrow, gap, text style and content - MLEADER uses it") {
    Fixture f;
    f.host->run("-MLEADERSTYLE", {"New", "Note", "Edit", "Note", "Arrow", "Tick", "Gap", "2",
                                  "Content", "None", "Done", "Done"});
    const DimStyle* d = f.doc.dimStyleByName("MLEADER-Note");
    REQUIRE(d);
    CHECK(d->arrowType == ArrowType::ArchTick);
    CHECK(d->textGap == doctest::Approx(2));
    CHECK(f.doc.header().custom.at("tbcad.mleaderstyle.Note") == "None");

    // Content defaults to None, so no note text is asked for.
    auto r = f.host->run("MLEADER", {"0,0", "10,10", ""});
    CHECK(r.finished);
    REQUIRE(f.leaders() == 1);
    f.doc.forEachEntity([&](const Entity& e) {
        if (auto* l = entityAs<MLeaderData>(e))
            CHECK(l->style == d->id);
    });
    CHECK(f.host->undo());
    CHECK(f.host->undo());
    CHECK(f.doc.dimStyleByName("MLEADER-Note") == nullptr);
}

TEST_CASE("MLEADERSTYLE Rename, Current, Delete and bad input") {
    Fixture f;
    f.host->run("-MLEADERSTYLE", {"New", "A", "New", "B", "Rename", "A", "C", "Done"});
    CHECK(f.doc.dimStyleByName("MLEADER-C"));
    CHECK(f.doc.dimStyleByName("MLEADER-A") == nullptr);
    f.host->run("-MLEADERSTYLE", {"Delete", "B", "Current", "Standard", "Done"});
    CHECK(f.doc.dimStyleByName("MLEADER-B") == nullptr);
    CHECK(!f.doc.header().custom.count("tbcad.mleaderstyle.B"));
    CHECK(!f.doc.header().custom.count("tbcad.mleadercurrent"));
    f.host->run("-MLEADERSTYLE",
                {"Edit", "C", "Content", "Sketch", "Arrow", "Round", "Done", "Done"});
    CHECK(f.host->logContains("Content must be"));
    CHECK(f.host->logContains("Arrow"));
    f.host->run("-MLEADERSTYLE", {"New", "Standard", "Done"});
    CHECK(f.host->logContains("already used"));
    f.host->run("-MLEADERSTYLE", {"Edit", "Nope", "Done"});
    CHECK(f.host->logContains("No multileader style"));
}

TEST_CASE("-MLEADERSTYLE sets arrow size and text height - MLEADERSTYLE opens the manager") {
    Fixture f;
    f.host->run("-MLEADERSTYLE",
                {"New", "Big", "Edit", "Big", "ArrowSize", "5", "TextHeight", "7", "Done", "Done"});
    const DimStyle* d = f.doc.dimStyleByName("MLEADER-Big");
    REQUIRE(d);
    CHECK(d->arrowSize == doctest::Approx(5));
    CHECK(d->textHeight == doctest::Approx(7));
    CHECK(dimcmd::mleaderStyleNames(f.doc) == std::vector<std::string>{"Big"});
    CHECK(dimcmd::currentMLeaderStyleName(f.doc) == "Big");
    CHECK(dimcmd::mleaderContent(f.doc, "Big") == "Text");
    CHECK(f.host->run("MLEADERSTYLE", {}).finished);
    CHECK(f.host->run("MLS", {}).finished);
}

TEST_CASE("MLEADERSTYLE block, annotative, leader points and dogleg length - MLEADER uses them") {
    Fixture f;
    {
        Transaction tx = f.doc.begin("block");
        BlockDef def;
        def.name = "TagCircle";
        REQUIRE(tx.addBlock(std::move(def)));
        tx.commit();
    }
    f.host->run(
        "-MLEADERSTYLE",
        {"New",       "Tag",        "Edit",      "Tag",       "Content", "Block",        "Block",
         "TagCircle", "Annotative", "Yes",       "MaxPoints", "3",       "DoglegLength", "10",
         "Block",     "Nope",       "MaxPoints", "1",         "Done",    "Done"});
    CHECK(f.host->logContains("no block named"));
    CHECK(f.host->logContains("MaxPoints must be"));
    const dimcmd::MLeaderStyle st = dimcmd::currentMLeaderStyle(f.doc);
    CHECK(st.block == "TagCircle");
    CHECK(st.annotative);
    CHECK(st.maxPoints == 3);
    CHECK(st.doglegLength == doctest::Approx(10));

    // Arrowhead, one bend, the landing; Enter takes the style's block.
    auto r = f.host->run("MLEADER", {"0,0", "10,10", "20,10", ""});
    CHECK(r.finished);
    const Entity* made = nullptr;
    f.doc.forEachEntity([&](const Entity& e) {
        if (entityAs<MLeaderData>(e))
            made = &e;
    });
    REQUIRE(made);
    const auto* m = entityAs<MLeaderData>(*made);
    CHECK(made->common.annotative);
    CHECK(m->content == MLeaderData::Content::Block);
    CHECK(f.doc.block(m->block)->name == "TagCircle");
    REQUIRE(m->lines.size() == 1);
    CHECK(m->lines[0].size() == 3);
    CHECK(m->contentPos.x == doctest::Approx(30));

    // The settings follow a rename and go with a delete.
    f.host->run("-MLEADERSTYLE", {"Rename", "Tag", "Tag2", "Done"});
    CHECK(dimcmd::currentMLeaderStyle(f.doc).block == "TagCircle");
    CHECK(!f.doc.header().custom.count("tbcad.mleaderopt.Tag"));
}
