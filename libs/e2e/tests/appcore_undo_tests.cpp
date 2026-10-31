// Random typed commands through AppCore with the real command set, mixed with Ctrl+Z / Ctrl+Y,
// Save and new commands that cut off the redo history. After every step the drawing must equal the
// state the history model expects, and what is derived from it (spatial index, scene, selection,
// modified flag) must agree with the drawing.
#include <doctest/doctest.h>

#include <algorithm>
#include <filesystem>
#include <random>

#include "tbcad/cmd/register_all.h"
#include "tbcad/core/dump.h"
#include "tbcad/ui/app_core.h"
#include "tbcad/ui/ui_registry.h"

using namespace tbcad;
namespace fs = std::filesystem;

namespace {

struct Session {
    fs::path dir;
    cmd::Registry registry;
    ui::UiRegistry uiRegistry;
    std::unique_ptr<ui::AppCore> core;

    explicit Session(unsigned seed) {
        dir = fs::temp_directory_path() / ("tbcad_appundo_" + std::to_string(seed));
        fs::remove_all(dir);
        fs::create_directories(dir);
        registerAllCommands(registry);
        core =
            std::make_unique<ui::AppCore>(registry, platform::AppPaths{dir / "cfg", dir / "data"});
        registerAllUi(uiRegistry);
        core->registerBuiltinUi(uiRegistry);
        core->applyUiKeybindings(uiRegistry);
        core->setViewportSize(1000, 800);
    }
    ~Session() {
        core.reset();
        std::error_code ec;
        fs::remove_all(dir, ec);
    }
    std::string dump() { return canonicalDump(core->document()); }
};

}  // namespace

TEST_CASE("random commands mixed with undo, redo and save keep the drawing and its views exact") {
    static const char* const kCommands[] = {
        "LINE 0,0 100,50 ",
        "LINE 20,20 -40,80 ",
        "CIRCLE 50,50 30",
        "CIRCLE -20,10 15",
        "RECTANG 0,0 60,40",
        "PLINE 0,0 50,0 50,50 ",
        "ARC 0,0 50,50 100,0",
        "MOVE 0,0 25,-10",
        "COPY 0,0 40,40 ",
        "ROTATE 0,0 33",
        "SCALE 0,0 1.5",
        "MIRROR 0,0 100,0 N",
        "OFFSET 10 5,5",
        "ERASE",
        "EXPLODE",
        "STRETCH 0,0 50,50 0,0 5,5",
    };
    for (unsigned seed = 1; seed <= 8; ++seed) {
        CAPTURE(seed);
        Session s(seed);
        std::mt19937 rng(seed * 7919u);
        std::vector<std::string> states{s.dump()};  // the history model: states[pos] is current
        std::size_t pos = 0;
        bool dirty = false;  // like AutoCAD: undoing back to the saved state stays modified
        const fs::path file = s.dir / "work.tbcad";

        auto checkDerived = [&] {
            const Document& d = s.core->document();
            CHECK(s.core->spatialIndex().size() == d.modelSpaceCount());
            CHECK(s.core->scene().entityCount() == d.modelSpaceCount());
            for (EntityId id : s.core->selection())
                CHECK(d.find(id) != nullptr);
            CHECK(s.core->modified() == dirty);
        };

        for (int step = 0; step < 60; ++step) {
            const unsigned what = rng() % 10;
            INFO("step " << step << " op " << what << " pos " << pos << " n " << states.size());
            if (what == 0) {
                const bool did = s.core->undo();
                CHECK(did == (pos > 0));
                if (did) {
                    --pos;
                    dirty = true;
                }
            } else if (what == 1) {
                const bool did = s.core->redo();
                CHECK(did == (pos + 1 < states.size()));
                if (did) {
                    ++pos;
                    dirty = true;
                }
            } else if (what == 2 && pos < states.size()) {
                REQUIRE(s.core->saveAs(file).ok());
                dirty = false;
            } else {
                std::vector<EntityId> pick;
                for (EntityId id : s.core->document().modelSpace())
                    if (rng() % 2)
                        pick.push_back(id);
                s.core->setSelection(pick);
                const std::string cmd = kCommands[rng() % (sizeof(kCommands) / sizeof(*kCommands))];
                INFO("command " << cmd);
                s.core->submitText(cmd);
                for (int guard = 0; s.core->runner().active() && guard < 3; ++guard)
                    s.core->submitText(guard % 2 ? "10,10" : "5,5");
                if (s.core->runner().active())
                    s.core->pressEscape();
                REQUIRE_FALSE(s.core->runner().active());
                const std::string now = s.dump();
                if (now != states[pos]) {
                    // A new edit throws the redo history away.
                    dirty = true;
                    states.resize(pos + 1);
                    states.push_back(now);
                    ++pos;
                    CHECK_FALSE(s.core->redo());
                }
            }
            CHECK(s.dump() == states[pos]);
            checkDerived();
        }
        CHECK(states.size() > 8);  // the commands really changed the drawing
        // Walk the whole history both ways.
        while (pos > 0) {
            REQUIRE(s.core->undo());
            --pos;
            dirty = true;  // even when the walk ends on the saved state
            CHECK(s.dump() == states[pos]);
        }
        CHECK_FALSE(s.core->undo());
        while (pos + 1 < states.size()) {
            REQUIRE(s.core->redo());
            ++pos;
            dirty = true;
            CHECK(s.dump() == states[pos]);
        }
        CHECK_FALSE(s.core->redo());
        checkDerived();
    }
}
