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
            (void)host.run("-PURGE", {"", "Yes"});
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

// Commands that talk to files, printers or the window rather than the drawing, and the undo
// commands themselves (the session below drives those on purpose).
bool skippedCommand(const std::string& name) {
    static const char* const kParts[] = {
        "EXPORT", "IMPORT", "SAVE", "PRINT", "PLOT",     "DEVGEN", "NEWFROM", "ATTACH",  "RELOAD",
        "OPEN",   "QUIT",   "EXIT", "NEW",   "TEMPLATE", "UNDO",   "REDO",    "ALIASES", "WBLOCK"};
    for (const char* part : kParts)
        if (name.find(part) != std::string::npos)
            return true;
    return name == "U";
}

std::vector<std::string> editingCommands(const cmd::Registry& reg) {
    std::vector<std::string> names;
    for (cmd::Command* c : reg.list())
        if (!skippedCommand(std::string(c->name())))
            names.emplace_back(c->name());
    std::sort(names.begin(), names.end());
    return names;
}

std::vector<ScriptItem> randomItems(std::mt19937& rng, const std::vector<EntityId>& ids,
                                    std::size_t n) {
    static const char* const kText[] = {"0,0",   "100,50", "@200,0", "1000,1000", "5",   "0",
                                        "-3",    "2",      "Yes",    "No",        "All", "Last",
                                        "Close", "A",      "x y",    "0.5",       "L1",  "B1"};
    std::vector<ScriptItem> out;
    for (std::size_t i = 0; i < n; ++i) {
        switch (rng() % 8) {
            case 0:
                out.push_back(ScriptItem::enter());
                break;
            case 1:
                out.push_back(ScriptItem::click({double(rng() % 20000), double(rng() % 20000)}));
                break;
            case 2:
                if (!ids.empty())
                    out.push_back(
                        ScriptItem::pick(ids[rng() % ids.size()], {double(rng() % 5000), 0}));
                break;
            case 3:
                if (!ids.empty())
                    out.push_back(
                        ScriptItem::select({ids[rng() % ids.size()], ids[rng() % ids.size()]}));
                break;
            default:
                out.push_back(ScriptItem(kText[rng() % std::size(kText)]));
        }
    }
    out.push_back(ScriptItem::esc());
    out.push_back(ScriptItem::esc());
    return out;
}

// One command of any group with random input: half the time one of the scripted ones above, half
// the time a random registered command fed random answers.
std::string anyStep(cmd::ScriptHost& host, const Document& doc, const std::vector<std::string>& all,
                    std::mt19937& rng, int step) {
    if (rng() % 2 == 0)
        return randomStep(host, doc, rng, step);
    std::vector<EntityId> ids = someIds(doc, rng, 6);
    const std::string& name = all[rng() % all.size()];
    host.setSelection(someIds(doc, rng, rng() % 4));
    (void)host.run(name, randomItems(rng, ids, 3 + rng() % 10));
    return name;
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

// The model of the history: states[d] is the drawing with d steps in the undo stack. A command that
// changes the drawing pushes one step (and drops the redo history); undo, redo, UNDOMARK, UNDOBACK
// and UNDOBEGIN / UNDOEND move along it.
TEST_CASE("200 random commands of every group, undo grouping and marks: the history is exact") {
    for (unsigned seed : {3u, 17u, 41u}) {
        CAPTURE(seed);
        std::mt19937 rng(seed);
        std::unique_ptr<Document> doc = loadSample();
        cmd::Registry reg;
        registerAllCommands(reg);
        cmd::ScriptHost host(*doc, reg);
        const std::vector<std::string> all = editingCommands(reg);
        REQUIRE(all.size() > 100);

        const std::string original = canonicalDump(*doc);
        std::vector<std::string> states{original};
        std::vector<std::size_t> marks;  // depths
        std::map<std::string, int> used;
        auto depth = [&] { return host.undoStack().undoCount(); };
        auto check = [&](const char* what, int step) {
            INFO("step " << step << " " << what);
            REQUIRE(depth() < states.size());
            CHECK(canonicalDump(*doc) == states[depth()]);
        };

        for (int step = 0; step < 200; ++step) {
            const std::size_t d0 = depth();
            const unsigned pick = rng() % 20;
            if (pick == 0 && d0 > 0) {
                REQUIRE(host.undo());
                REQUIRE(depth() == d0 - 1);
                check("undo", step);
            } else if (pick == 1 && host.undoStack().canRedo()) {
                REQUIRE(host.redo());
                REQUIRE(depth() == d0 + 1);
                check("redo", step);
            } else if (pick == 2 && d0 > 0) {
                (void)host.run("UNDOMARK", {});
                if (marks.empty() || marks.back() != d0)
                    marks.push_back(d0);
            } else if (pick == 3) {
                (void)host.run("UNDOBACK", {});
                if (marks.empty()) {
                    REQUIRE(depth() == d0);  // no mark: nothing happens
                } else {
                    REQUIRE(depth() == marks.back());
                    marks.pop_back();
                    check("undo back", step);
                }
            } else if (pick == 4) {
                // a group of several commands undoes as one step
                (void)host.run("UNDOBEGIN", {});
                const int n = 2 + static_cast<int>(rng() % 4);
                for (int i = 0; i < n; ++i) {
                    (void)anyStep(host, *doc, all, rng, step);
                    (void)host.feed({ScriptItem::esc(), ScriptItem::esc()});
                }
                (void)host.run("UNDOEND", {});
                REQUIRE_FALSE(host.undoStack().inGroup());
                if (depth() != d0) {
                    CHECK(depth() == d0 + 1);
                    states.resize(d0 + 1);
                    states.push_back(canonicalDump(*doc));
                    while (!marks.empty() && marks.back() > d0)
                        marks.pop_back();
                } else {
                    check("empty group", step);
                }
                continue;
            } else {
                const std::string name = anyStep(host, *doc, all, rng, step);
                (void)host.feed({ScriptItem::esc(), ScriptItem::esc()});
                ++used[name];
                INFO("step " << step << " " << name);
                if (depth() == d0) {
                    CHECK(canonicalDump(*doc) == states[d0]);  // no step: nothing changed
                } else {
                    REQUIRE(depth() == d0 + 1);
                    states.resize(d0 + 1);
                    states.push_back(canonicalDump(*doc));
                    while (!marks.empty() && marks.back() > d0)
                        marks.pop_back();
                }
            }
            // undo past a mark removes it
            while (!marks.empty() && marks.back() > depth())
                marks.pop_back();
        }
        CHECK(used.size() >= 25);  // a real mix of commands
        CHECK(depth() > 10);       // the exact count shifts whenever a command gains an option

        // Back through everything and forward again, exactly.
        const std::size_t top = depth();
        const std::string end = canonicalDump(*doc);
        REQUIRE(end == states[top]);
        while (host.undo()) {
        }
        CHECK(canonicalDump(*doc) == original);
        std::size_t redone = 0;
        while (host.redo())
            ++redone;
        CHECK(redone >= top);
        CHECK(canonicalDump(*doc) == states[depth()]);
    }
}

TEST_CASE("every command: undo gives the original drawing back, redo gives its result") {
    cmd::Registry reg;
    registerAllCommands(reg);
    const std::vector<std::string> all = editingCommands(reg);
    std::mt19937 rng(20261018);
    for (const std::string& name : all) {
        INFO("command " << name);
        for (int round = 0; round < 3; ++round) {
            std::unique_ptr<Document> doc = loadSample();
            cmd::ScriptHost host(*doc, reg);
            std::vector<EntityId> ids = someIds(*doc, rng, 30);
            const std::string before = canonicalDump(*doc);
            std::vector<ScriptItem> items;
            if (round == 0)
                items = {"0,0",
                         "1000,0",
                         "1000,1000",
                         "0,1000",
                         ScriptItem::enter(),
                         ScriptItem::enter(),
                         ScriptItem::esc()};
            else
                items = randomItems(rng, ids, 6 + rng() % 14);
            host.setSelection(someIds(*doc, rng, 1 + rng() % 3));
            (void)host.run(name, items);
            (void)host.feed({ScriptItem::esc(), ScriptItem::esc()});
            const std::string after = canonicalDump(*doc);
            while (host.undo()) {
            }
            CHECK(canonicalDump(*doc) == before);
            while (host.redo()) {
            }
            CHECK(canonicalDump(*doc) == after);
        }
    }
}

TEST_CASE("the undo history stays inside its memory limit and what is left still undoes exactly") {
    std::mt19937 rng(77);
    std::unique_ptr<Document> doc = loadSample();
    cmd::Registry reg;
    registerAllCommands(reg);
    cmd::ScriptHost host(*doc, reg);
    const std::vector<std::string> all = editingCommands(reg);
    const std::size_t limit = 16 * 1024;
    host.undoStack().setLimitBytes(limit);

    std::vector<std::string> states{canonicalDump(*doc)};  // by number of steps ever pushed
    std::size_t peak = 0;
    for (int step = 0; step < 200; ++step) {
        (void)anyStep(host, *doc, all, rng, step);
        (void)host.feed({ScriptItem::esc(), ScriptItem::esc()});
        const std::size_t pushed = host.undoStack().droppedSteps() + host.undoStack().undoCount();
        REQUIRE(pushed >= states.size() - 1);
        REQUIRE(pushed <= states.size());  // a command pushes at most one step
        if (pushed == states.size())
            states.push_back(canonicalDump(*doc));
        else
            CHECK(canonicalDump(*doc) == states.back());  // no step: nothing changed
        peak = std::max(peak, host.undoStack().usedBytes());
        CHECK(host.undoStack().usedBytes() <= limit);
    }
    CHECK(peak > 0);
    // The oldest steps were dropped: what remains undoes back to the state just before them.
    const std::size_t left = host.undoStack().undoCount();
    REQUIRE(left < states.size());
    CHECK(host.undoStack().droppedSteps() > 0);  // the limit really was reached
    while (host.undo()) {
    }
    CHECK(canonicalDump(*doc) == states[states.size() - 1 - left]);
}
