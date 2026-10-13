#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>

#include "tbcad/platform/keybindings.h"

using namespace tbcad::platform;
namespace fs = std::filesystem;

TEST_CASE("chords parse in any case and order") {
    auto c = parseChord("shift+CTRL+z");
    REQUIRE(c);
    CHECK(c->ctrl);
    CHECK(c->shift);
    CHECK_FALSE(c->alt);
    CHECK(c->key == "Z");
    CHECK(c->toString() == "Ctrl+Shift+Z");
    CHECK(parseChord("Esc")->key == "ESCAPE");
    CHECK(parseChord("F5")->toString() == "F5");
    CHECK(parseChord("Ctrl+Delete")->toString() == "Ctrl+Delete");
    CHECK_FALSE(parseChord("Ctrl+"));
    CHECK_FALSE(parseChord("Hyper+Q"));
    CHECK_FALSE(parseChord("F99"));
    CHECK_FALSE(parseChord(""));
}

TEST_CASE("defaults, overrides and lookup by chord") {
    Keybindings kb;
    kb.declare("edit.undo", "Ctrl+Z");
    kb.declare("edit.redo", "Ctrl+Y");
    kb.declare("view.extents", "");
    CHECK(kb.label("edit.undo") == "Ctrl+Z");
    CHECK(kb.label("view.extents").empty());
    CHECK(kb.actionFor(*parseChord("ctrl+y")) == "edit.redo");
    kb.set("edit.redo", "Ctrl+Shift+Z");
    CHECK(kb.actionFor(*parseChord("ctrl+y")).empty());
    CHECK(kb.actionFor(*parseChord("Ctrl+Shift+Z")) == "edit.redo");
}

TEST_CASE("keybindings.json overrides defaults and reports problems") {
    fs::path dir = fs::temp_directory_path() / "tbcad_platform_kb";
    fs::remove_all(dir);
    fs::create_directories(dir);
    {
        std::ofstream(dir / "keybindings.json")
            << R"({"edit.undo": "Ctrl+U", "edit.redo": "Ctrl+Q", "bogus": "F1", "x": "Hyper+1", "file.open": "Ctrl+Q"})";
    }
    Keybindings kb;
    kb.declare("edit.undo", "Ctrl+Z");
    kb.declare("edit.redo", "Ctrl+Y");
    kb.declare("file.open", "Ctrl+O");
    auto warnings = kb.load(dir / "keybindings.json");
    REQUIRE(warnings.ok());
    CHECK(warnings.value().size() == 3);  // unknown action, bad chord, duplicate chord
    CHECK(kb.label("edit.undo") == "Ctrl+U");

    REQUIRE(kb.save(dir / "out.json").ok());
    Keybindings again;
    again.declare("edit.undo", "Ctrl+Z");
    REQUIRE(again.load(dir / "out.json").ok());
    CHECK(again.label("edit.undo") == "Ctrl+U");
}

TEST_CASE("a missing keybindings file is fine; a broken one is an error") {
    fs::path dir = fs::temp_directory_path() / "tbcad_platform_kb2";
    fs::remove_all(dir);
    fs::create_directories(dir);
    Keybindings kb;
    kb.declare("a", "F1");
    CHECK(kb.load(dir / "missing.json").ok());
    {
        std::ofstream(dir / "bad.json") << "[1,2";
    }
    CHECK_FALSE(kb.load(dir / "bad.json").ok());
    CHECK(kb.label("a") == "F1");
}
