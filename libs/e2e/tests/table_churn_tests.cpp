// Every registered command runs between edits that grow and rewrite the drawing's tables (new
// layers, blocks and styles; every layer, block and style row replaced) and its objects. A command
// or a reactor that kept a pointer into a row or an object across such an edit would read freed
// memory; with a sanitizer build (and TBCAD_HARDEN_EDITS, which reallocates object data on every
// edit) that is reported, and without one the drawing must still undo, redo and round trip.
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

std::vector<EntityId> modelIds(const Document& doc) {
    std::vector<EntityId> ids = doc.modelSpace();
    if (ids.size() > 300)
        ids.resize(300);
    return ids;
}

// New rows in every table, then every layer, block, style row and some objects rewritten.
void churn(Document& doc, int round, std::mt19937& rng) {
    Transaction tx = doc.begin("churn");
    for (int i = 0; i < 12; ++i) {
        Layer l;
        l.name = "CHURN-" + std::to_string(round) + "-" + std::to_string(i);
        (void)tx.addLayer(l);
        TextStyle ts;
        ts.name = "CHURN-TS-" + std::to_string(round) + "-" + std::to_string(i);
        (void)tx.addTextStyle(ts);
    }
    for (int i = 0; i < 4; ++i) {
        auto b = tx.addBlock(
            BlockDef{.name = "CHURN-B-" + std::to_string(round) + "-" + std::to_string(i)});
        if (b.ok())
            (void)tx.add(Entity{{}, LineData{{0, 0}, {double(i + 1), 0}}}, b.value());
    }
    std::vector<LayerId> layers;
    for (const auto& [id, l] : doc.layers())
        layers.push_back(id);
    for (LayerId id : layers)
        (void)tx.modifyLayer(id, [&](Layer& l) { l.description += "."; });
    std::vector<BlockId> blocks;
    for (const auto& [id, b] : doc.blocks())
        blocks.push_back(id);
    for (BlockId id : blocks)
        (void)tx.modifyBlock(id, [](BlockDef&) {});  // the row is replaced all the same
    std::vector<TextStyleId> styles;
    for (const auto& [id, s] : doc.textStyles())
        styles.push_back(id);
    for (TextStyleId id : styles)
        (void)tx.modifyTextStyle(id, [](TextStyle& s) { s.widthFactor = s.widthFactor; });
    const std::vector<EntityId> ids = modelIds(doc);
    for (int i = 0; i < 40 && !ids.empty(); ++i)
        (void)tx.modify(ids[rng() % ids.size()],
                        [&](Entity& e) { e.common.color = Color::index(1 + rng() % 200); });
    tx.commit();
}

std::vector<ScriptItem> script(std::mt19937& rng, const std::vector<EntityId>& ids) {
    static const char* const kText[] = {"0,0",  "1000,0", "@500,500", "All", "Last",
                                        "Undo", "Yes",    "2",        "45",  "Close"};
    std::vector<ScriptItem> out;
    const int n = 6 + static_cast<int>(rng() % 10);
    for (int i = 0; i < n; ++i) {
        switch (rng() % 6) {
            case 0:
                out.push_back(ScriptItem::enter());
                break;
            case 1:
                if (!ids.empty())
                    out.push_back(ScriptItem::pick(ids[rng() % ids.size()]));
                break;
            case 2:
                if (!ids.empty())
                    out.push_back(ScriptItem::select({ids[rng() % ids.size()],
                                                      ids[rng() % ids.size()],
                                                      ids[rng() % ids.size()]}));
                break;
            default:
                out.push_back(ScriptItem(kText[rng() % std::size(kText)]));
        }
    }
    out.push_back(ScriptItem::esc());
    out.push_back(ScriptItem::esc());
    return out;
}

}  // namespace

TEST_CASE("every command between table growth and row rewrites, then undo, redo and round trip") {
    cmd::Registry probe;
    registerAllCommands(probe);
    std::vector<std::string> names;
    for (cmd::Command* c : probe.list())
        names.emplace_back(c->name());
    REQUIRE(names.size() > 100);

    auto dir = std::filesystem::temp_directory_path() / ("tbcad_churn_" + std::to_string(getpid()));
    std::filesystem::create_directories(dir);
    std::mt19937 rng(20261019);
    for (const std::string& name : names) {
        if (skipped(name))
            continue;
        INFO("command " << name);
        std::unique_ptr<Document> doc = loadSample();
        cmd::ScriptHost host(*doc, probe);
        for (int round = 0; round < 3; ++round) {
            churn(*doc, round, rng);
            (void)host.run(name, script(rng, modelIds(*doc)));
        }
        while (host.undo()) {
        }
        while (host.redo()) {
        }
        while (host.undo()) {
        }
        const auto file = dir / (name + ".tbcad");
        REQUIRE(io::writeTbcad(*doc, file).ok());
        io::IoReport report;
        auto back = io::readTbcad(file, report);
        REQUIRE(back.ok());
        CHECK(canonicalDump(*back.value()) == canonicalDump(*doc));
    }
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}
