// The UNDO command itself: a number of steps, Mark / Back, Begin / End groups, and redo after each,
// with groups that change the layer table and geometry together. After every move along the
// history the drawing must equal the recorded state, and undoing everything must give back, also
// after a save and reload, the drawing we started from.
#include <doctest/doctest.h>

#include <filesystem>
#include <string>
#include <vector>

#include "tbcad/cmd/cmd.h"
#include "tbcad/cmd/register_all.h"
#include "tbcad/cmd/script_host.h"
#include "tbcad/core/dump.h"
#include "tbcad/io/tbcad.h"

using namespace tbcad;
using cmd::ScriptItem;

namespace {

std::unique_ptr<Document> loadSample() {
    io::IoReport report;
    auto r = io::readTbcad(std::filesystem::path(TBCAD_SAMPLES_DIR) / "thai-house.tbcad", report);
    REQUIRE(r.ok());
    return std::move(r).value();
}

// Saves and reloads the drawing. The id counters in the file header keep growing after an undo
// on purpose (an id is never reused), so the bytes of the file may differ; the content must not.
std::string savedDump(const Document& doc, const char* name) {
    const auto file = std::filesystem::temp_directory_path() / name;
    REQUIRE(io::writeTbcad(doc, file).ok());
    io::IoReport report;
    auto back = io::readTbcad(file, report);
    REQUIRE(back.ok());
    std::error_code ec;
    std::filesystem::remove(file, ec);
    return canonicalDump(*back.value());
}

// One step that changes the layer table and the geometry together.
void layerAndLine(cmd::ScriptHost& host, int n) {
    const std::string layer = "UO" + std::to_string(n);
    (void)host.run("-LAYER", {"New", layer, "Color", "3", layer, ScriptItem::enter()});
    (void)host.run("-LAYER", {"Make", layer, ScriptItem::enter()});
    (void)host.run("LINE", {std::to_string(n * 100) + ",0", std::to_string(n * 100) + ",500",
                            ScriptItem::enter()});
}

}  // namespace

TEST_CASE("UNDO N, Mark, Back and Begin / End, with redo, restore exact states") {
    std::unique_ptr<Document> doc = loadSample();
    cmd::Registry reg;
    registerAllCommands(reg);
    cmd::ScriptHost host(*doc, reg);
    const std::string startDump = canonicalDump(*doc);
    auto depth = [&] { return host.undoStack().undoCount(); };

    std::vector<std::string> states{startDump};
    auto record = [&] { states.push_back(canonicalDump(*doc)); };

    // Four ordinary steps, each a layer and a line.
    for (int i = 1; i <= 4; ++i) {
        (void)host.run("-LAYER", {"New", "UO" + std::to_string(i), ScriptItem::enter()});
        record();
        (void)host.run("LINE", {std::to_string(i * 100) + ",0", std::to_string(i * 100) + ",500",
                                ScriptItem::enter()});
        record();
    }
    REQUIRE(depth() == 8);

    SUBCASE("UNDO with a number goes back that many steps, and stops at the start") {
        (void)host.run("UNDO", {"3"});
        CHECK(depth() == 5);
        CHECK(canonicalDump(*doc) == states[5]);
        (void)host.run("UNDO", {ScriptItem::enter()});  // default is one step
        CHECK(depth() == 4);
        CHECK(canonicalDump(*doc) == states[4]);
        (void)host.run("UNDO", {"99"});
        CHECK(depth() == 0);
        CHECK(canonicalDump(*doc) == startDump);
        CHECK(savedDump(*doc, "tbcad_undo_opt_back.tbcad") == startDump);
        // Redo walks forward through the same states.
        for (std::size_t i = 1; i < states.size(); ++i) {
            REQUIRE(host.redo());
            CHECK(canonicalDump(*doc) == states[i]);
        }
        CHECK_FALSE(host.redo());
    }

    SUBCASE("UNDO 0 and a negative number change nothing") {
        (void)host.run("UNDO", {"0", ScriptItem::esc()});
        (void)host.run("UNDO", {"-2", ScriptItem::esc()});
        CHECK(depth() == 8);
        CHECK(canonicalDump(*doc) == states[8]);
    }

    SUBCASE("UNDO Mark then UNDO Back returns to the mark, and redo returns past it") {
        (void)host.run("UNDO", {"Mark"});
        layerAndLine(host, 5);
        layerAndLine(host, 6);
        const std::size_t top = depth();
        REQUIRE(top > 8);
        const std::string topDump = canonicalDump(*doc);
        (void)host.run("UNDO", {"Back"});
        CHECK(depth() == 8);
        CHECK(canonicalDump(*doc) == states[8]);
        while (host.redo()) {
        }
        CHECK(depth() == top);
        CHECK(canonicalDump(*doc) == topDump);
    }

    SUBCASE("UNDO Back without a mark and answering No changes nothing") {
        (void)host.run("UNDO", {"Back", "No"});
        CHECK(depth() == 8);
        CHECK(canonicalDump(*doc) == states[8]);
    }

    SUBCASE("UNDO Back without a mark and answering Yes undoes everything, saved copy identical") {
        (void)host.run("UNDO", {"Back", "Yes"});
        CHECK(depth() == 0);
        CHECK(canonicalDump(*doc) == startDump);
        CHECK(savedDump(*doc, "tbcad_undo_opt_all.tbcad") == startDump);
    }

    SUBCASE("UNDO BEgin / End makes layer and geometry changes one step, redo restores both") {
        (void)host.run("UNDO", {"BEgin"});
        layerAndLine(host, 5);
        layerAndLine(host, 6);
        (void)host.run("UNDO", {"End"});
        REQUIRE_FALSE(host.undoStack().inGroup());
        CHECK(depth() == 9);
        const std::string grouped = canonicalDump(*doc);
        (void)host.run("UNDO", {ScriptItem::enter()});
        CHECK(depth() == 8);
        CHECK(canonicalDump(*doc) == states[8]);
        REQUIRE(host.redo());
        CHECK(canonicalDump(*doc) == grouped);
        // Undo everything and compare the file bytes.
        (void)host.run("UNDO", {"99"});
        CHECK(canonicalDump(*doc) == startDump);
        CHECK(savedDump(*doc, "tbcad_undo_opt_grp.tbcad") == startDump);
    }

    SUBCASE("UNDO End with no open group says so and changes nothing") {
        (void)host.run("UNDO", {"End"});
        CHECK(depth() == 8);
        CHECK(canonicalDump(*doc) == states[8]);
    }

    SUBCASE("a plain undo while a group is open closes the group and undoes it whole") {
        (void)host.run("UNDO", {"BEgin"});
        layerAndLine(host, 5);
        layerAndLine(host, 6);
        (void)host.run("U", {});
        CHECK_FALSE(host.undoStack().inGroup());
        CHECK(depth() == 8);
        CHECK(canonicalDump(*doc) == states[8]);
    }

    SUBCASE("a new command after an undo drops the redo history") {
        (void)host.run("UNDO", {"2"});
        REQUIRE(host.undoStack().canRedo());
        layerAndLine(host, 7);
        CHECK_FALSE(host.undoStack().canRedo());
        CHECK_FALSE(host.redo());
    }

    SUBCASE("UNDO Auto and Control are accepted and the history stays whole") {
        (void)host.run("UNDO", {"Auto", "OFF"});
        (void)host.run("UNDO", {"Control", "None"});
        CHECK(depth() == 8);
        (void)host.run("UNDO", {"2"});
        CHECK(canonicalDump(*doc) == states[6]);
    }
}
