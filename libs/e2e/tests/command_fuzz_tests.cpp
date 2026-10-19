// Every registered command, run on the Thai house sample with scripted and random input. A command
// may refuse, cancel or report an error, but it must not crash or throw, and the drawing must
// still save and load afterwards.
#include <doctest/doctest.h>

#include <algorithm>
#include <filesystem>
#include <random>
#include <string>
#include <vector>

#include "tbcad/cmd/cmd.h"
#include "tbcad/cmd/register_all.h"
#include "tbcad/cmd/script_host.h"
#include "tbcad/core/dump.h"
#include "tbcad/io/tbcad.h"

#ifdef _WIN32
#include <process.h>
#define getpid _getpid
#else
#include <unistd.h>
#endif

using namespace tbcad;
using cmd::ScriptItem;

namespace {

// Commands that talk to files, printers or the window rather than the drawing.
bool skipped(const std::string& name) {
    static const char* const kParts[] = {"EXPORT", "IMPORT",  "SAVE",   "PRINT",   "PLOT",
                                         "DEVGEN", "NEWFROM", "ATTACH", "RELOAD",  "OPEN",
                                         "QUIT",   "EXIT",    "NEW",    "TEMPLATE"};
    for (const char* part : kParts)
        if (name.find(part) != std::string::npos)
            return true;
    return false;
}

std::unique_ptr<Document> loadSample() {
    io::IoReport report;
    auto r = io::readTbcad(std::filesystem::path(TBCAD_SAMPLES_DIR) / "thai-house.tbcad", report);
    REQUIRE(r.ok());
    return std::move(r).value();
}

std::vector<ScriptItem> randomItems(std::mt19937& rng, const std::vector<EntityId>& ids,
                                    std::size_t n) {
    static const char* const kText[] = {"0,0",  "100,50", "@200,0", "1000,1000", "5",   "0",
                                        "-3",   "1e9",    "2",      "Yes",       "No",  "All",
                                        "Last", "Undo",   "Close",  "A",         "x y", "0.5"};
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

#ifdef TBCAD_FUZZ_ROUNDS
constexpr int kRounds = TBCAD_FUZZ_ROUNDS;
#else
constexpr int kRounds = 4;
#endif

}  // namespace

TEST_CASE("every command survives scripted and random input on the Thai house sample") {
    cmd::Registry probe;
    registerAllCommands(probe);
    std::vector<std::string> names;
    for (cmd::Command* c : probe.list())
        names.emplace_back(c->name());
    REQUIRE(names.size() > 100);

    auto dir = std::filesystem::temp_directory_path() / ("tbcad_fuzz_" + std::to_string(getpid()));
    std::filesystem::create_directories(dir);

    std::mt19937 rng(20261013);
    for (const std::string& name : names) {
        if (skipped(name))
            continue;
        INFO("command " << name);
        for (int round = 0; round < kRounds; ++round) {
            std::unique_ptr<Document> doc = loadSample();
            cmd::ScriptHost host(*doc, probe);
            std::vector<EntityId> ids;
            doc->forEachEntity([&](const Entity& e) { ids.push_back(e.common.id); });
            std::sort(ids.begin(), ids.end());
            if (ids.size() > 400)
                ids.resize(400);

            std::vector<ScriptItem> items;
            switch (round) {
                case 0:  // cancelled at once
                    items = {ScriptItem::esc()};
                    break;
                case 1:  // plain points
                    items = {"0,0",
                             "1000,0",
                             "1000,1000",
                             "0,1000",
                             ScriptItem::enter(),
                             ScriptItem::enter(),
                             ScriptItem::esc()};
                    break;
                default:
                    items = randomItems(rng, ids, 6 + rng() % 14);
            }
            (void)host.run(name, items);
            // Undo and redo what it did, then check the drawing still round-trips.
            while (host.undo()) {
            }
            const auto file = dir / (name + ".tbcad");
            REQUIRE(io::writeTbcad(*doc, file).ok());
            io::IoReport report;
            auto back = io::readTbcad(file, report);
            REQUIRE(back.ok());
            CHECK(canonicalDump(*back.value()) == canonicalDump(*doc));
        }
    }
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}
