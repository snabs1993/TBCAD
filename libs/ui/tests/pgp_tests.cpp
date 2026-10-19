#include <doctest/doctest.h>

#include <fstream>

#include "tbcad/ui/app_core.h"
#include "tbcad/ui/settings_model.h"
#include "test_support.h"

using namespace tbcad;
using namespace tbcad::ui;
using namespace tbcad::ui::test;
namespace fs = std::filesystem;

TEST_CASE("parsePgp reads aliases, comments and skips what it cannot use") {
    const char* text =
        "; AutoCAD alias file\n"
        "\n"
        "[Command]\n"
        "L,        *LINE   ; a comment\n"
        "cc,\t*circle\n"
        "BAD LINE\n"
        "X-1,   notstar\n"
        "A B, *ARC\n"
        "XL,*LINE,\n"
        "[External Commands]\n"
        "NOTEPAD,  START NOTEPAD, 1,\n";
    PgpParse p = parsePgp(text);
    REQUIRE(p.aliases.size() == 3);
    CHECK(p.aliases.at("L") == "LINE");
    CHECK(p.aliases.at("CC") == "CIRCLE");
    CHECK(p.aliases.at("XL") == "LINE");
    CHECK(p.skipped.size() == 3);  // no comma, no star, bad alias; external section ignored
}

TEST_CASE("importPgp adds valid aliases, reports unknown commands and the ones already in effect") {
    auto reg = makeRegistry();
    AliasEditor ed(*reg, {});
    AliasImportReport rep =
        ed.importPgp("TT, *TESTLINE\nTL, *TESTLINE\nQQ, *NOSUCHCMD\nTE,*TESTERASE\n");
    CHECK(rep.error.empty());
    CHECK(rep.added == 2);
    REQUIRE(rep.skipped.size() == 2);  // TL already an alias; NOSUCHCMD
    CHECK(reg->find("tt")->name() == "TESTLINE");
    CHECK(reg->find("te")->name() == "TESTERASE");
    CHECK(reg->find("qq") == nullptr);
}

TEST_CASE("exportPgp writes the effective aliases and importing it into a fresh registry works") {
    auto reg = makeRegistry();
    AliasEditor ed(*reg, {});
    REQUIRE(ed.set("ZZ", "TESTLINE").empty());
    const std::string text = ed.exportPgp();
    CHECK(text.find("ZZ,") != std::string::npos);
    CHECK(text.find("*TESTLINE") != std::string::npos);

    auto other = makeRegistry();
    AliasEditor ed2(*other, {});
    AliasImportReport rep = ed2.importPgp(text);
    CHECK(rep.error.empty());
    CHECK(other->find("zz")->name() == "TESTLINE");
}

TEST_CASE("ALIASIMPORT and ALIASEXPORT run as commands and the import is saved") {
    fs::path dir = tempDir("pgp-cmd");
    auto reg = makeRegistry();
    AppCore core(*reg, platform::AppPaths{dir / "cfg", dir / "data"});
    { std::ofstream(dir / "in.pgp") << "QL, *TESTLINE\n"; }
    core.submitText("ALIASIMPORT");
    core.submitText((dir / "in.pgp").string());
    CHECK(reg->find("ql")->name() == "TESTLINE");
    CHECK(fs::exists(dir / "cfg" / "aliases.json"));
    core.submitText("ALIASEXPORT");
    core.submitText((dir / "out.pgp").string());
    std::ifstream in(dir / "out.pgp");
    std::string out((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    CHECK(out.find("QL,") != std::string::npos);

    core.submitText("ALIASIMPORT");
    core.submitText((dir / "missing.pgp").string());
    CHECK(core.history().back().find("Cannot read") != std::string::npos);
}

TEST_CASE("ALIASEDIT sets, lists and removes one alias and saves it") {
    fs::path dir = tempDir("aliasedit");
    auto reg = makeRegistry();
    AppCore core(*reg, platform::AppPaths{dir / "cfg", dir / "data"});
    for (const char* line : {"ALIASEDIT", "Set", "qq", "TESTLINE"})
        core.submitText(line);
    CHECK(reg->find("qq")->name() == "TESTLINE");
    CHECK(fs::exists(dir / "cfg" / "aliases.json"));
    for (const char* line : {"ALIASEDIT", "List", "TESTLINE"})
        core.submitText(line);
    bool listed = false;
    for (const std::string& h : core.history())
        listed = listed || h.find("QQ -> TESTLINE") != std::string::npos;
    CHECK(listed);
    for (const char* line : {"ALIASEDIT", "Set", "zz", "NOSUCHCOMMAND"})
        core.submitText(line);
    CHECK(reg->find("zz") == nullptr);
    for (const char* line : {"ALIASEDIT", "Remove", "qq"})
        core.submitText(line);
    CHECK(reg->find("qq") == nullptr);
}

TEST_CASE("key binding profiles export, import and reject a bad file") {
    fs::path dir = tempDir("keys-profile");
    auto reg = makeRegistry();
    AppCore core(*reg, platform::AppPaths{dir / "cfg", dir / "data"});
    KeybindingEditor ed(core.keybindings(), dir / "cfg" / "keybindings.json");
    REQUIRE(ed.set("file.new", "Ctrl+Shift+N").ok);
    REQUIRE(core.exportKeybindings(dir / "profile.json").empty());

    REQUIRE(ed.reset("file.new").ok);
    CHECK(core.keybindings().label("file.new") == "Ctrl+N");
    REQUIRE(core.importKeybindings(dir / "profile.json").empty());
    CHECK(core.keybindings().label("file.new") == "Ctrl+Shift+N");

    CHECK_FALSE(core.importKeybindings(dir / "missing.json").empty());
    { std::ofstream(dir / "bad.json") << "{ not json"; }
    CHECK_FALSE(core.importKeybindings(dir / "bad.json").empty());
    CHECK(core.keybindings().label("file.new") == "Ctrl+Shift+N");  // unchanged
}
