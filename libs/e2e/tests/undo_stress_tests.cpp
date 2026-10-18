// Long random sequences of drawing, editing, table and block commands. After every command the
// drawing is recorded; then undo walks back through every recorded state, redo walks forward
// through them again, and a save and reload in the middle must give the same drawing.
#include <doctest/doctest.h>

#include <algorithm>
#include <filesystem>
#include <map>
#include <random>
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

std::string pt(std::mt19937& rng, int range) {
    return std::to_string(static_cast<int>(rng() % (2 * range)) - range) + "," +
           std::to_string(static_cast<int>(rng() % (2 * range)) - range);
}

std::vector<EntityId> someIds(const Document& doc, std::mt19937& rng, std::size_t n) {
    std::vector<EntityId> all;
    doc.forEachEntity([&](const Entity& e) { all.push_back(e.common.id); });
    std::sort(all.begin(), all.end());
    std::vector<EntityId> out;
    for (std::size_t i = 0; i < n && !all.empty(); ++i)
        out.push_back(all[rng() % all.size()]);
    return out;
}

// Runs one random command. Returns its name for failure messages.
std::string randomStep(cmd::ScriptHost& host, const Document& doc, std::mt19937& rng, int step) {
    const std::string tag = std::to_string(step);
    switch (rng() % 22) {
        case 0:
            (void)host.run("LINE", {pt(rng, 5000), pt(rng, 5000), ScriptItem::enter()});
            return "LINE";
        case 1:
            (void)host.run("CIRCLE", {pt(rng, 5000), std::to_string(10 + rng() % 500)});
            return "CIRCLE";
        case 2:
            (void)host.run("RECTANGLE", {pt(rng, 5000), pt(rng, 5000)});
            return "RECTANGLE";
        case 3:
            host.setSelection(someIds(doc, rng, 1 + rng() % 4));
            (void)host.run("MOVE", {"0,0", pt(rng, 3000)});
            return "MOVE";
        case 4:
            host.setSelection(someIds(doc, rng, 1 + rng() % 4));
            (void)host.run("ROTATE", {pt(rng, 3000), std::to_string(rng() % 360)});
            return "ROTATE";
        case 5:
            host.setSelection(someIds(doc, rng, 1 + rng() % 3));
            (void)host.run("SCALE", {pt(rng, 3000), std::to_string(0.5 + (rng() % 30) / 10.0)});
            return "SCALE";
        case 6:
            host.setSelection(someIds(doc, rng, 1 + rng() % 3));
            (void)host.run("MIRROR", {pt(rng, 3000), pt(rng, 3000), ScriptItem::enter()});
            return "MIRROR";
        case 7:
            host.setSelection(someIds(doc, rng, 1 + rng() % 3));
            (void)host.run("COPY", {"0,0", pt(rng, 2000), ScriptItem::enter()});
            return "COPY";
        case 8:
            host.setSelection(someIds(doc, rng, 1 + rng() % 3));
            (void)host.run("ERASE", {});
            return "ERASE";
        case 9:
            (void)host.run("-LAYER", {"New", "L" + tag, ScriptItem::enter()});
            return "-LAYER New";
        case 10:
            (void)host.run("-LAYER",
                           {"Make", "L" + std::to_string(rng() % (step + 1)), ScriptItem::enter()});
            return "-LAYER Make";
        case 11:
            (void)host.run("-LAYER",
                           {"Color", std::to_string(1 + rng() % 250),
                            "L" + std::to_string(rng() % (step + 1)), ScriptItem::enter()});
            return "-LAYER Color";
        case 12:
            host.setSelection(someIds(doc, rng, 1 + rng() % 3));
            (void)host.run("BLOCK", {"B" + std::to_string(rng() % 4), pt(rng, 100),
                                     ScriptItem::enter(), "Yes"});
            return "BLOCK";
        case 13:
            (void)host.run("INSERT", {"B" + std::to_string(rng() % 4), pt(rng, 4000)});
            return "INSERT";
        case 14:
            (void)host.run("DIMLINEAR", {pt(rng, 3000), pt(rng, 3000), pt(rng, 3000)});
            return "DIMLINEAR";
        case 15:
            (void)host.run("HATCH", {pt(rng, 3000), ScriptItem::enter()});
            return "HATCH";
        case 16:
            (void)host.run("PURGE", {"", "Yes"});
            return "PURGE";
        case 17:
            (void)host.run("RENAME",
                           {"Layer", "L" + std::to_string(rng() % (step + 1)), "R" + tag});
            return "RENAME layer";
        case 18:
            (void)host.run("RENAME", {"Block", "B" + std::to_string(rng() % 4), "BR" + tag});
            return "RENAME block";
        case 19: {
            auto ids = someIds(doc, rng, 2);
            if (ids.size() == 2) {
                (void)host.run("LAYMRG", {ScriptItem::select({ids[0]}), ScriptItem::enter(),
                                          ScriptItem::pick(ids[1], {0, 0})});
            }
            return "LAYMRG";
        }
        case 20:
            host.setSelection(someIds(doc, rng, 1 + rng() % 3));
            (void)host.run("STRETCH", {pt(rng, 3000), pt(rng, 3000), "0,0", pt(rng, 500)});
            return "STRETCH";
        default:
            (void)host.run("TEXT", {pt(rng, 4000), "20", "0", "stress " + tag});
            return "TEXT";
    }
}

}  // namespace

TEST_CASE("undo and redo walk through every state of a long random session") {
    for (unsigned seed : {11u, 29u, 53u, 97u, 131u, 173u}) {
        CAPTURE(seed);
        std::mt19937 rng(seed);
        std::unique_ptr<Document> doc = loadSample();
        cmd::Registry reg;
        registerAllCommands(reg);
        cmd::ScriptHost host(*doc, reg);

        std::vector<std::string> states{canonicalDump(*doc)};
        std::vector<std::string> names{"start"};
        const int steps = 60;
        for (int step = 0; step < steps; ++step) {
            const std::size_t before = host.undoStack().undoCount();
            const std::string name = randomStep(host, *doc, rng, step);
            (void)host.feed({ScriptItem::esc(), ScriptItem::esc()});
            const std::size_t after = host.undoStack().undoCount();
            const std::string now = canonicalDump(*doc);
            INFO("step " << step << " " << name);
            if (after == before) {
                // No undo entry: the drawing must be exactly as it was.
                CHECK(now == states.back());
            } else {
                CHECK(after == before + 1);
                states.push_back(now);
                names.push_back(name);
            }
            if (step == steps / 2) {
                const auto file = std::filesystem::temp_directory_path() /
                                  ("tbcad_undo_stress_" + std::to_string(seed) + ".tbcad");
                REQUIRE(io::writeTbcad(*doc, file).ok());
                io::IoReport report;
                auto back = io::readTbcad(file, report);
                REQUIRE(back.ok());
                CHECK(canonicalDump(*back.value()) == now);
                std::error_code ec;
                std::filesystem::remove(file, ec);
            }
        }
        const std::string endState = states.back();
        {
            std::map<std::string, int> hist;
            for (const auto& n : names)
                ++hist[n];
            CHECK(hist.size() >= 8);  // a real mix of commands
        }
        CHECK(states.size() > 15);  // the session really changed the drawing

        // Back through every state.
        for (std::size_t i = states.size() - 1; i > 0; --i) {
            INFO("undoing " << names[i]);
            REQUIRE(host.undo());
            CHECK(canonicalDump(*doc) == states[i - 1]);
        }
        CHECK_FALSE(host.undoStack().canUndo());
        // And forward again.
        for (std::size_t i = 1; i < states.size(); ++i) {
            INFO("redoing " << names[i]);
            REQUIRE(host.redo());
            CHECK(canonicalDump(*doc) == states[i]);
        }
        CHECK(canonicalDump(*doc) == endState);
    }
}
