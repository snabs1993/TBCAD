// Semicolon scripts through AppCore, as --script and the PC gate run them: "ERASE;ALL;" erases
// everything, and each edit is taken back with U (UNDO asks how many steps).
#include <doctest/doctest.h>

#include <filesystem>
#include <string>

#include "tbcad/cmd/register_all.h"
#include "tbcad/ui/app_core.h"
#include "tbcad/ui/ui_registry.h"

using namespace tbcad;

namespace {

struct ScriptRig {
    std::filesystem::path dir;
    cmd::Registry registry;
    std::unique_ptr<ui::AppCore> core;

    ScriptRig() {
        dir = std::filesystem::temp_directory_path() / "tbcad_script_gate";
        std::filesystem::remove_all(dir);
        std::filesystem::create_directories(dir);
        registerAllCommands(registry);
        core =
            std::make_unique<ui::AppCore>(registry, platform::AppPaths{dir / "cfg", dir / "data"});
        core->setViewportSize(1000, 800);
        auto tx = core->document().begin("setup");
        for (int i = 0; i < 20; ++i)
            (void)tx.add(Entity{{}, LineData{{0, i * 5.0}, {100, i * 5.0}}});
        tx.commit();
    }
    ~ScriptRig() {
        core.reset();
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }
    std::size_t count() const { return core->document().modelSpaceCount(); }
    // A sum over every end point: changes with any move, turn or scale of the lines.
    double checksum() const {
        double sum = 0;
        core->document().forEachEntity([&](const Entity& e) {
            if (const auto* l = entityAs<LineData>(e))
                sum += l->a.x + 2 * l->a.y + 3 * l->b.x + 4 * l->b.y;
        });
        return sum;
    }
};

}  // namespace

TEST_CASE("ERASE ALL as a script erases everything, and U and REDO take it back and forth") {
    ScriptRig r;
    r.core->submitScript("ERASE;ALL;");
    CHECK_FALSE(r.core->runner().active());
    CHECK(r.count() == 0);
    r.core->submitText("U");
    CHECK(r.count() == 20);
    r.core->submitText("REDO");
    CHECK(r.count() == 0);
    r.core->submitText("U");
    CHECK(r.count() == 20);
}

TEST_CASE("the gate's edits run one after another and each is undone with U") {
    ScriptRig r;
    r.core->selectAll();  // the gate measures frames with everything selected first
    for (const char* edit :
         {"MOVE;ALL;;0,0;100,0", "ROTATE;ALL;;0,0;30", "SCALE;ALL;;0,0;1.5", "ERASE;ALL;"}) {
        const std::string name = edit;
        CAPTURE(name);
        r.core->clearSelection();
        const double x0 = r.checksum();
        r.core->submitScript(edit);
        CHECK_FALSE(r.core->runner().active());
        if (std::string(edit).rfind("ERASE", 0) == 0)
            CHECK(r.count() == 0);
        else
            CHECK(r.checksum() != doctest::Approx(x0));
        r.core->submitText("U");
        CHECK(r.count() == 20);
        CHECK(r.checksum() == doctest::Approx(x0));
        r.core->submitText("REDO");
        r.core->submitText("U");
        CHECK_FALSE(r.core->runner().active());
        CHECK(r.count() == 20);
    }
}

TEST_CASE("UNDO in a script waits for its count: a command name there is refused, not run") {
    ScriptRig r;
    r.core->submitScript("ERASE;ALL;");
    r.core->submitText("UNDO");
    CHECK(r.core->runner().active());  // "Enter the number of operations to undo"
    r.core->submitText("ERASE");       // not a number or option: asked again
    CHECK(r.core->runner().active());
    r.core->submitText("");  // Enter: one step
    CHECK_FALSE(r.core->runner().active());
    CHECK(r.count() == 20);
}
