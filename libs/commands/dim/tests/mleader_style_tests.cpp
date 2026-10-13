#include <doctest/doctest.h>

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
            if (entityAs<LeaderData>(e))
                ++n;
        });
        return n;
    }
};
}  // namespace

TEST_CASE("MLEADERSTYLE New/Edit sets arrow, gap, text style and content; MLEADER uses it") {
    Fixture f;
    f.host->run("MLEADERSTYLE", {"New", "Note", "Edit", "Note", "Arrow", "Tick", "Gap", "2",
                                 "Content", "None", "Done", "Done"});
    const DimStyle* d = f.doc.dimStyleByName("MLEADER-Note");
    REQUIRE(d);
    CHECK(d->arrowType == ArrowType::ArchTick);
    CHECK(d->textGap == doctest::Approx(2));
    CHECK(f.doc.header().custom.at("tbcad.mleaderstyle.Note") == "None");

    // Content defaults to None, so no note text is asked for.
    auto r = f.host->run("MLEADER", {"0,0", "10,10", "", ""});
    CHECK(r.finished);
    REQUIRE(f.leaders() == 1);
    f.doc.forEachEntity([&](const Entity& e) {
        if (auto* l = entityAs<LeaderData>(e))
            CHECK(l->style == d->id);
    });
    CHECK(f.host->undo());
    CHECK(f.host->undo());
    CHECK(f.doc.dimStyleByName("MLEADER-Note") == nullptr);
}

TEST_CASE("MLEADERSTYLE Rename, Current, Delete and bad input") {
    Fixture f;
    f.host->run("MLS", {"New", "A", "New", "B", "Rename", "A", "C", "Done"});
    CHECK(f.doc.dimStyleByName("MLEADER-C"));
    CHECK(f.doc.dimStyleByName("MLEADER-A") == nullptr);
    f.host->run("MLS", {"Delete", "B", "Current", "Standard", "Done"});
    CHECK(f.doc.dimStyleByName("MLEADER-B") == nullptr);
    CHECK(!f.doc.header().custom.count("tbcad.mleaderstyle.B"));
    CHECK(!f.doc.header().custom.count("tbcad.mleadercurrent"));
    f.host->run("MLS", {"Edit", "C", "Content", "Sketch", "Arrow", "Round", "Done", "Done"});
    CHECK(f.host->logContains("Content must be"));
    CHECK(f.host->logContains("Arrow"));
    f.host->run("MLS", {"New", "Standard", "Done"});
    CHECK(f.host->logContains("already used"));
    f.host->run("MLS", {"Edit", "Nope", "Done"});
    CHECK(f.host->logContains("No multileader style"));
}
