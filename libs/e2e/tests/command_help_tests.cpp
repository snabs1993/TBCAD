// The offline command help: every page belongs to a real command, and HELP / ? on the command
// line print a page, search the pages and ask the window to show a page.
#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <sstream>

#include "tbcad/cmd/register_all.h"
#include "tbcad/ui/app_core.h"

using namespace tbcad;

namespace {

struct HelpRig {
    std::filesystem::path dir;
    cmd::Registry registry;
    std::unique_ptr<ui::AppCore> core;

    HelpRig() {
        dir = std::filesystem::temp_directory_path() / "tbcad_command_help";
        std::filesystem::remove_all(dir);
        std::filesystem::create_directories(dir);
        registerAllCommands(registry);
        core =
            std::make_unique<ui::AppCore>(registry, platform::AppPaths{dir / "cfg", dir / "data"});
        std::ifstream in(TBCAD_COMMAND_HELP_FILE, std::ios::binary);
        std::ostringstream buf;
        buf << in.rdbuf();
        core->help().parse(buf.str());
    }
    ~HelpRig() {
        core.reset();
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }
};

}  // namespace

TEST_CASE("every command help page belongs to a real command and is complete") {
    HelpRig rig;
    REQUIRE(rig.core->help().entries().size() >= 40);
    for (const cmd::HelpEntry& e : rig.core->help().entries()) {
        INFO("help page for " << e.name);
        CHECK(rig.registry.find(e.name) != nullptr);
        CHECK(!e.summary.empty());
        CHECK(!e.usage.empty());
        CHECK(!e.example.empty());
    }
}

TEST_CASE("HELP and ? print a page, search the pages and ask for the page window") {
    HelpRig rig;
    auto last = [&] { return rig.core->history().back(); };
    rig.core->submitText("HELP OFFSET");
    CHECK(rig.core->takeHelpRequest() == "OFFSET");
    CHECK(rig.core->takeHelpRequest().empty());
    rig.core->submitText("? o");  // an alias finds the page of its command
    CHECK(rig.core->takeHelpRequest() == "OFFSET");
    rig.core->submitText("? parallel");
    CHECK(rig.core->history()[rig.core->history().size() - 2].find("OFFSET") != std::string::npos);
    CHECK(rig.core->takeHelpRequest().empty());
    rig.core->submitText("help zzzqqq");
    CHECK(last().find("No help page") != std::string::npos);
    rig.core->submitText("HELP");
    CHECK(last().find("Type HELP followed") != std::string::npos);
    CHECK_FALSE(rig.core->runner().active());
}
