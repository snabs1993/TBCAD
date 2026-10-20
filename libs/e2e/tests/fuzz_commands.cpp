// libFuzzer target: commands run on the Thai house sample with input made from the fuzzer's bytes.
// Each input is a few commands in a row, each with its clicks, picks and typed answers; then
// everything is undone. A command may refuse, cancel or report an error, but it must not crash,
// and the drawing must still save and load as it was.
//
//   cmake -DTBCAD_CMD_FUZZ=ON ...; tbcad_cmd_fuzz $PWD/corpus $PWD/tests/corpus/fuzz/commands
// (absolute paths: the fuzzer moves to a temporary folder)
//
// Commands that talk to files, printers or the window are left out; the process works in a
// temporary folder so a file a command writes anyway does not land in the source tree.
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
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

bool skipped(const std::string& name) {
    static const char* const kParts[] = {"EXPORT", "IMPORT",    "SAVE",    "PRINT",    "PLOT",
                                         "DEVGEN", "NEWFROM",   "ATTACH",  "RELOAD",   "OPEN",
                                         "QUIT",   "EXIT",      "NEW",     "TEMPLATE", "WBLOCK",
                                         "SCRIPT", "ETRANSMIT", "ARCHIVE", "XREF",     "IMAGE"};
    for (const char* part : kParts)
        if (name.find(part) != std::string::npos)
            return true;
    return false;
}

struct World {
    cmd::Registry registry;
    std::vector<std::string> names;
    std::vector<std::uint8_t> sample;
    std::string sampleDump;
};

World& world() {
    static World* w = [] {
        auto* out = new World;
        registerAllCommands(out->registry);
        for (cmd::Command* c : out->registry.list())
            if (!skipped(std::string(c->name())))
                out->names.emplace_back(c->name());
        std::sort(out->names.begin(), out->names.end());
        io::IoReport report;
        auto doc =
            io::readTbcad(std::filesystem::path(TBCAD_SAMPLES_DIR) / "thai-house.tbcad", report);
        if (!doc.ok())
            std::abort();
        auto bytes = io::writeTbcadToMemory(*doc.value());
        if (!bytes.ok())
            std::abort();
        out->sample = std::move(bytes).value();
        out->sampleDump = canonicalDump(*doc.value());
        const auto dir = std::filesystem::temp_directory_path() / "tbcad_cmd_fuzz";
        std::filesystem::create_directories(dir);
        std::filesystem::current_path(dir);
        return out;
    }();
    return *w;
}

// Reads the fuzzer's bytes in order; past the end everything is zero.
struct Reader {
    const std::uint8_t* p;
    std::size_t n;
    std::uint8_t byte() {
        if (n == 0)
            return 0;
        --n;
        return *p++;
    }
    std::uint16_t word() { return static_cast<std::uint16_t>(byte() | (byte() << 8)); }
    double coord() { return (static_cast<int>(word()) - 16384) * 2.0; }  // about +-32 m
};

ScriptItem item(Reader& in, const std::vector<EntityId>& ids) {
    static const char* const kText[] = {
        "0,0", "100,50", "@200,0", "@500<45", "1000,1000", "5",    "0",     "-3", "1e9",
        "2",   "Yes",    "No",     "All",     "Last",      "Undo", "Close", "A",  "x y",
        "0.5", "-1e-12", "nan",    "inf",     "'ZOOM",     "E",    "*",     "P",  "W",
        "C",   "F",      "1,1,1",  "@",       "<90",       "360",  "1e308", "",   "Window"};
    switch (in.byte() % 10) {
        case 0:
            return ScriptItem::enter();
        case 1:
        case 2: {
            const double x = in.coord(), y = in.coord();
            return ScriptItem::click({x, y});
        }
        case 3:
            if (!ids.empty()) {
                const EntityId id = ids[in.word() % ids.size()];
                const double x = in.coord(), y = in.coord();
                return ScriptItem::pick(id, {x, y});
            }
            return ScriptItem::enter();
        case 4: {
            std::vector<EntityId> pick;
            const int count = 1 + in.byte() % 8;
            for (int i = 0; i < count && !ids.empty(); ++i)
                pick.push_back(ids[in.word() % ids.size()]);
            return ScriptItem::select(std::move(pick));
        }
        case 5: {  // a few raw characters
            std::string s;
            const int len = in.byte() % 12;
            for (int i = 0; i < len; ++i)
                s.push_back(static_cast<char>(in.byte()));
            return ScriptItem(std::move(s));
        }
        case 6:
            return ScriptItem::esc();
        default:
            return ScriptItem(kText[in.byte() % std::size(kText)]);
    }
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    World& w = world();
    if (w.names.empty())
        return 0;
    Reader in{data, size};
    io::IoReport report;
    auto read = io::readTbcad(std::span<const std::uint8_t>(w.sample), report);
    if (!read.ok())
        std::abort();
    std::unique_ptr<Document> doc = std::move(read).value();
    std::vector<EntityId> ids;
    doc->forEachEntity([&](const Entity& e) { ids.push_back(e.common.id); });
    std::sort(ids.begin(), ids.end());

    cmd::ScriptHost host(*doc, w.registry);
    const int commands = 1 + in.byte() % 4;
    for (int c = 0; c < commands && in.n > 0; ++c) {
        const std::string& name = w.names[in.word() % w.names.size()];
        std::vector<ScriptItem> items;
        const int count = in.byte() % 24;
        for (int i = 0; i < count; ++i)
            items.push_back(item(in, ids));
        items.push_back(ScriptItem::esc());
        items.push_back(ScriptItem::esc());
        if (std::getenv("TBCAD_FUZZ_ECHO"))
            std::fprintf(stderr, "command %s, %zu items\n", name.c_str(), items.size());
        (void)host.run(name, items);
    }
    // Whatever it did must save and load as it is.
    auto bytes = io::writeTbcadToMemory(*doc);
    if (!bytes.ok())
        std::abort();
    io::IoReport back;
    auto again = io::readTbcad(std::span<const std::uint8_t>(bytes.value()), back);
    if (!again.ok()) {
        std::fprintf(stderr, "saved drawing does not load: %s\n", again.error().message.c_str());
        std::abort();
    }
    if (const std::string a = canonicalDump(*doc), b = canonicalDump(*again.value()); a != b) {
        if (std::getenv("TBCAD_FUZZ_ECHO")) {
            std::FILE* f = std::fopen("before.txt", "wb");
            std::fwrite(a.data(), 1, a.size(), f);
            std::fclose(f);
            f = std::fopen("after.txt", "wb");
            std::fwrite(b.data(), 1, b.size(), f);
            std::fclose(f);
        }
        std::abort();
    }
    while (host.undo()) {
    }
    if (std::getenv("TBCAD_FUZZ_UNDO_CHECK") && canonicalDump(*doc) != w.sampleDump)
        std::abort();
    return 0;
}
