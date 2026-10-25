#include <doctest/doctest.h>

#include <fstream>
#include <sstream>

#include "tbcad/platform/json.h"
#include "tbcad/ui/settings_model.h"
#include "test_support.h"

using namespace tbcad;
using namespace tbcad::ui;
namespace fs = std::filesystem;

namespace {

class Dummy final : public cmd::Command {
public:
    Dummy(std::string name, std::vector<std::string> aliases)
        : name_(std::move(name)), aliases_(std::move(aliases)) {}
    std::string_view name() const override { return name_; }
    std::vector<std::string> defaultAliases() const override { return aliases_; }
    cmd::Task run(cmd::CommandContext&) override { co_return; }

private:
    std::string name_;
    std::vector<std::string> aliases_;
};

struct Reg {
    cmd::Registry reg;
    Reg() {
        (void)reg.add(std::make_unique<Dummy>("LINE", std::vector<std::string>{"L"}));
        (void)reg.add(std::make_unique<Dummy>("LOFT", std::vector<std::string>{"L", "LF"}));
        (void)reg.add(std::make_unique<Dummy>("CIRCLE", std::vector<std::string>{"C"}));
        (void)reg.add(std::make_unique<Dummy>("COPY", std::vector<std::string>{}));
    }
};

const cmd::AliasEntry* row(const std::vector<cmd::AliasEntry>& rows, const std::string& alias) {
    for (const auto& r : rows)
        if (r.alias == alias)
            return &r;
    return nullptr;
}

std::string slurp(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::ostringstream b;
    b << in.rdbuf();
    return b.str();
}

}  // namespace

TEST_CASE("alias table marks built-in clashes and user overrides") {
    Reg r;
    AliasEditor ed(r.reg, {});
    auto rows = ed.rows();
    const auto* l = row(rows, "L");
    REQUIRE(l);
    CHECK(l->conflict);
    CHECK(l->command.empty());
    CHECK(l->note.find("LINE") != std::string::npos);
    CHECK(l->note.find("LOFT") != std::string::npos);
    const auto* c = row(rows, "C");
    REQUIRE(c);
    CHECK_FALSE(c->conflict);
    CHECK(c->command == "CIRCLE");
    CHECK(c->source == cmd::AliasEntry::Source::BuiltIn);

    CHECK(ed.set("l", "line").empty());  // settles the clash
    rows = ed.rows();
    l = row(rows, "L");
    REQUIRE(l);
    CHECK_FALSE(l->conflict);
    CHECK(l->command == "LINE");
    CHECK(l->source == cmd::AliasEntry::Source::User);
    CHECK(r.reg.find("L") == r.reg.find("LINE"));

    CHECK(ed.set("C", "COPY").empty());  // overrides a built-in
    rows = ed.rows();
    CHECK(row(rows, "C")->note.find("overrides the built-in CIRCLE") != std::string::npos);
    CHECK(ed.remove("C").empty());
    rows = ed.rows();
    CHECK(row(rows, "C")->command == "CIRCLE");
    CHECK(row(rows, "C")->source == cmd::AliasEntry::Source::BuiltIn);
}

TEST_CASE("alias edits are validated and written to aliases.json") {
    Reg r;
    fs::path file = test::tempDir("alias_editor") / "cfg" / "aliases.json";
    AliasEditor ed(r.reg, file);
    CHECK(ed.set("", "LINE") == "Type the alias first.");
    CHECK(ed.set("X", "") == "Choose the command the alias runs.");
    CHECK(ed.set("X", "NOSUCH").find("Unknown command") == 0);
    CHECK(ed.set("LINE", "COPY") != "");  // a command name cannot be an alias
    CHECK(r.reg.userAliases().empty());
    CHECK_FALSE(fs::exists(file));

    CHECK(ed.set("CP", "COPY").empty());
    CHECK(ed.set("lf2", "LOFT").empty());
    auto parsed = platform::parseJson(slurp(file));
    REQUIRE(parsed);
    CHECK(parsed.value().find("CP")->asString() == "COPY");
    CHECK(parsed.value().find("LF2")->asString() == "LOFT");
    CHECK(ed.remove("nothere") == "That is not a user alias.");
    CHECK(ed.remove("cp").empty());
    parsed = platform::parseJson(slurp(file));
    CHECK(parsed.value().find("CP") == nullptr);

    // The file is what ALIASES reloads.
    Reg r2;
    r2.reg.loadUserAliases(file);
    CHECK(r2.reg.find("LF2") == r2.reg.find("LOFT"));
}

TEST_CASE("keybinding editor: conflicts, take over, unbind, reset, file") {
    platform::Keybindings keys;
    keys.declare("file.save", "Ctrl+S");
    keys.declare("file.saveAs", "Ctrl+Shift+S");
    keys.declare("view.log", "");
    fs::path file = test::tempDir("key_editor") / "cfg" / "keybindings.json";
    KeybindingEditor ed(keys, file);

    auto rows = ed.rows();
    REQUIRE(rows.size() == 3);

    auto o = ed.set("view.log", "control+l");
    CHECK(o.ok);
    CHECK(keys.label("view.log") == "Ctrl+L");
    CHECK(slurp(file).find("\"view.log\": \"Ctrl+L\"") != std::string::npos);

    o = ed.set("view.log", "Ctrl+S");  // taken by file.save
    CHECK_FALSE(o.ok);
    CHECK(o.conflictWith == "file.save");
    CHECK(keys.label("view.log") == "Ctrl+L");
    CHECK(keys.label("file.save") == "Ctrl+S");

    o = ed.set("view.log", "Ctrl+S", true);
    CHECK(o.ok);
    CHECK(keys.label("file.save").empty());
    CHECK(keys.label("view.log") == "Ctrl+S");

    CHECK(ed.set("nope", "F2").error.find("Unknown action") == 0);
    CHECK(ed.set("view.log", "Banana+Q").error.find("not a key combination") != std::string::npos);

    CHECK(ed.reset("file.save").ok);
    // file.save is back on Ctrl+S, which view.log also uses: flagged in the rows.
    bool flagged = false;
    for (const auto& row : ed.rows())
        flagged = flagged || (row.action == "file.save" && row.conflictWith == "view.log");
    CHECK(flagged);
    CHECK(ed.set("view.log", "").ok);  // unbind
    CHECK(keys.label("view.log").empty());
    for (const auto& row : ed.rows())
        CHECK(row.conflictWith.empty());
}

TEST_CASE("keybinding editor: command shortcuts") {
    platform::Keybindings keys;
    keys.declare("file.save", "Ctrl+S");
    fs::path file = test::tempDir("key_editor_cmd") / "keybindings.json";
    KeybindingEditor ed(keys, file);

    CHECK_FALSE(ed.addCommand("  ", "F4").ok);
    CHECK(ed.addCommand(" LAYER ", "ctrl+shift+l").ok);
    CHECK(keys.label("cmd:LAYER") == "Ctrl+Shift+L");
    CHECK(keys.actionFor(*platform::parseChord("Ctrl+Shift+L")) == "cmd:LAYER");

    auto o = ed.addCommand("ZOOM E", "Ctrl+S");
    CHECK_FALSE(o.ok);
    CHECK(o.conflictWith == "file.save");
    CHECK(keys.label("cmd:ZOOM E").empty());
    bool leaked = false;
    for (const auto& a : keys.actions())
        leaked = leaked || a == "cmd:ZOOM E";
    CHECK_FALSE(leaked);

    // Saved and read back by a fresh instance without an "unknown action" warning.
    platform::Keybindings again;
    again.declare("file.save", "Ctrl+S");
    auto warn = again.load(file);
    REQUIRE(warn);
    CHECK(warn.value().empty());
    CHECK(again.label("cmd:LAYER") == "Ctrl+Shift+L");

    CHECK_FALSE(ed.removeCommand("file.save").ok);
    CHECK(ed.removeCommand("cmd:LAYER").ok);
    CHECK(keys.label("cmd:LAYER").empty());
    CHECK(slurp(file).find("cmd:LAYER") == std::string::npos);
}
