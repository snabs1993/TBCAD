// DXF is the only format TBCAD saves in, so whatever a command leaves in the drawing must come
// back from a saved DXF. Every command runs with scripted and random input on the sample and on
// an empty drawing; the result is saved as DXF, opened again and compared.
#include <doctest/doctest.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <random>
#include <string>
#include <vector>

#include "../../dxf/tests/writer/semantic_dump.h"
#include "tbcad/cmd/cmd.h"
#include "tbcad/cmd/register_all.h"
#include "tbcad/cmd/script_host.h"
#include "tbcad/dxf/reader.h"
#include "tbcad/dxf/writer.h"
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

bool skippedForSave(const std::string& name) {
    static const char* const kParts[] = {"EXPORT", "IMPORT",  "SAVE",   "PRINT",    "PLOT",
                                         "DEVGEN", "NEWFROM", "ATTACH", "RELOAD",   "OPEN",
                                         "QUIT",   "EXIT",    "NEW",    "TEMPLATE", "WBLOCK"};
    for (const char* part : kParts)
        if (name.find(part) != std::string::npos)
            return true;
    return false;
}

std::unique_ptr<Document> saveAndOpen(const Document& doc, std::string& error) {
    dxf::Report r1;
    auto out = dxf::exportDxf(doc, dxf::ExportOptions{}, r1);
    if (!out.ok()) {
        error = "export failed";
        return nullptr;
    }
    const std::string& s = out.value().bytes;
    std::vector<std::byte> bytes(s.size());
    std::memcpy(bytes.data(), s.data(), s.size());
    dxf::Report r2;
    auto back = dxf::importDxf(bytes, dxf::ImportOptions{}, r2);
    if (!back.ok()) {
        error = "import failed";
        return nullptr;
    }
    return std::move(back.value().document);
}

std::vector<ScriptItem> randomSaveItems(std::mt19937& rng, const std::vector<EntityId>& ids,
                                        std::size_t n) {
    static const char* const kText[] = {"0,0", "100,50", "@200,0", "1000,1000", "5",   "250",
                                        "30",  "2",      "Yes",    "No",        "All", "Last",
                                        "A",   "abc",    "0.5",    "@0,300",    "45",  "C"};
    std::vector<ScriptItem> out;
    for (std::size_t i = 0; i < n; ++i) {
        switch (rng() % 8) {
            case 0:
                out.push_back(ScriptItem::enter());
                break;
            case 1:
            case 5:
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
    out.push_back(ScriptItem::enter());
    out.push_back(ScriptItem::esc());
    out.push_back(ScriptItem::esc());
    return out;
}

}  // namespace

TEST_CASE("what a command leaves in the drawing comes back from a saved DXF" *
          doctest::skip(std::getenv("TBCAD_DXF_SAVE_FUZZ") == nullptr)) {
    cmd::Registry probe;
    registerAllCommands(probe);
    std::vector<std::string> names;
    for (cmd::Command* c : probe.list())
        names.emplace_back(c->name());
    const int rounds = std::atoi(std::getenv("TBCAD_DXF_SAVE_FUZZ"));

    auto dir = std::filesystem::temp_directory_path() / ("tbcad_dxffuzz_" + std::to_string(getpid()));
    std::filesystem::create_directories(dir);
    const auto startDir = std::filesystem::current_path();
    std::filesystem::current_path(dir);

    std::map<std::string, std::string> firstDiff;
    const char* only = std::getenv("TBCAD_DXF_SAVE_FUZZ_ONLY");
    for (const std::string& name : names) {
        if (skippedForSave(name) || (only && name != only))
            continue;
        for (int round = 0; round < rounds; ++round) {
            // One seed per command and round, so a failure replays alone.
            unsigned seed = 20261115u + static_cast<unsigned>(round) * 7919u;
            for (char ch : name)
                seed = seed * 131u + static_cast<unsigned char>(ch);
            std::mt19937 rng(seed);
            std::unique_ptr<Document> doc;
            if (round % 2 == 0) {
                io::IoReport report;
                auto r = io::readTbcad(
                    std::filesystem::path(TBCAD_SAMPLES_DIR) / "thai-house.tbcad", report);
                REQUIRE(r.ok());
                doc = std::move(r).value();
            } else {
                doc = std::make_unique<Document>();
            }
            // Start from a drawing that already is what a DXF gives back.
            std::string error;
            doc = saveAndOpen(*doc, error);
            REQUIRE(doc);
            cmd::ScriptHost host(*doc, probe);
            std::vector<EntityId> ids;
            doc->forEachEntity([&](const Entity& e) { ids.push_back(e.common.id); });
            std::sort(ids.begin(), ids.end());
            if (ids.size() > 400)
                ids.resize(400);
            (void)host.run(name, randomSaveItems(rng, ids, 6 + rng() % 14));
            std::unique_ptr<Document> back = saveAndOpen(*doc, error);
            std::string diff = error;
            if (back)
                diff = dxf::testing::SemanticDump::diff(dxf::testing::SemanticDump(*doc),
                                                        dxf::testing::SemanticDump(*back));
            if (!diff.empty() && only && !firstDiff.count(name)) {  // DEBUG
                dxf::Report rr;
                auto o2 = dxf::exportDxf(*doc, dxf::ExportOptions{}, rr);
                FILE* f = std::fopen("/tmp/fuzz_dump.dxf", "wb");
                std::fwrite(o2.value().bytes.data(), 1, o2.value().bytes.size(), f);
                std::fclose(f);
            }
            if (!diff.empty() && !firstDiff.count(name))
                firstDiff[name] = "round " + std::to_string(round) + ": " + diff.substr(0, only ? 20000 : 600);
        }
    }
    std::filesystem::current_path(startDir);
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    for (const auto& [name, diff] : firstDiff)
        MESSAGE("DXFDIFF " << name << " " << diff);
    CHECK(firstDiff.empty());
}
