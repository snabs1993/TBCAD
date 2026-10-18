#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>

#include "tbcad/cmd/cmd.h"

using namespace tbcad;
using namespace tbcad::cmd;

namespace {

class Named final : public Command {
public:
    Named(std::string n, std::vector<std::string> a)
        : name_(std::move(n)), aliases_(std::move(a)) {}
    std::string_view name() const override { return name_; }
    std::vector<std::string> defaultAliases() const override { return aliases_; }
    Task run(CommandContext&) override { co_return; }

private:
    std::string name_;
    std::vector<std::string> aliases_;
};

std::filesystem::path tempDir(const std::string& tag) {
    auto p = std::filesystem::temp_directory_path() / ("tbcad_cmd_" + tag);
    std::filesystem::remove_all(p);
    std::filesystem::create_directories(p);
    return p;
}

void write(const std::filesystem::path& p, const std::string& text) {
    std::ofstream(p, std::ios::binary) << text;
}

struct Fx {
    Registry reg;
    Fx() {
        REQUIRE(reg.add(std::make_unique<Named>("LINE", std::vector<std::string>{"L"})));
        REQUIRE(reg.add(std::make_unique<Named>("CIRCLE", std::vector<std::string>{"C", "CI"})));
    }
};

}  // namespace

TEST_CASE("built-in aliases and case-insensitive lookup") {
    Fx f;
    CHECK(f.reg.find("line") == f.reg.find("L"));
    CHECK(f.reg.find("l") != nullptr);
    CHECK(f.reg.find("ci")->name() == "CIRCLE");
    CHECK(f.reg.find("nothing") == nullptr);
    CHECK(f.reg.list().size() == 2);
    CHECK(f.reg.list()[0]->name() == "CIRCLE");  // sorted by name
}

TEST_CASE("invalid and duplicate names are refused") {
    Fx f;
    CHECK_FALSE(f.reg.add(std::make_unique<Named>("lower", std::vector<std::string>{})));
    CHECK_FALSE(f.reg.add(std::make_unique<Named>("", std::vector<std::string>{})));
    CHECK_FALSE(f.reg.add(std::make_unique<Named>("LINE", std::vector<std::string>{})));
    CHECK(f.reg.add(std::make_unique<Named>("-LAYER", std::vector<std::string>{})));
}

TEST_CASE("built-in alias conflicts are reported and neither side gets the alias") {
    Registry reg;
    REQUIRE(reg.add(std::make_unique<Named>("ARC", std::vector<std::string>{"A"})));
    REQUIRE(reg.add(std::make_unique<Named>("ARRAY", std::vector<std::string>{"A", "AR"})));
    CHECK(reg.find("A") == nullptr);
    CHECK(reg.find("AR")->name() == "ARRAY");
    REQUIRE_FALSE(reg.conflicts().empty());
    CHECK(reg.conflicts()[0].find("Alias A") != std::string::npos);
    // A user alias resolves the conflict.
    auto rep = reg.setUserAliases({{"A", "ARC"}});
    CHECK(rep.errors.empty());
    CHECK(reg.find("A")->name() == "ARC");
}

TEST_CASE("alias file overrides the defaults") {
    Fx f;
    auto dir = tempDir("ok");
    write(dir / "aliases.json", R"({"L": "CIRCLE", "cc": "circle", "XL": "LINE"})");
    AliasReport r = f.reg.loadUserAliases(dir / "aliases.json");
    CHECK(r.errors.empty());
    CHECK(r.userFileLoaded);
    CHECK(r.userAliases == 3);
    CHECK(f.reg.find("L")->name() == "CIRCLE");  // overridden
    CHECK(f.reg.find("CC")->name() == "CIRCLE");
    CHECK(f.reg.find("xl")->name() == "LINE");
    CHECK(f.reg.find("C")->name() == "CIRCLE");  // untouched default
    CHECK(f.reg.aliasesOf("circle").size() == 4);
}

TEST_CASE("a broken alias file reports an error and falls back to the defaults") {
    Fx f;
    auto dir = tempDir("broken");
    // First load a good file, then break it and reload: defaults must win.
    write(dir / "aliases.json", R"({"L": "CIRCLE"})");
    f.reg.loadUserAliases(dir / "aliases.json");
    CHECK(f.reg.find("L")->name() == "CIRCLE");

    for (const char* bad : {R"({"L": "LINE")", R"([1,2])", R"({"L": 5})", "", R"({"L": "LINE"} x)",
                            R"({L: "LINE"})"}) {
        write(dir / "aliases.json", bad);
        AliasReport r = f.reg.loadUserAliases(dir / "aliases.json");
        INFO("input: " << bad);
        REQUIRE(r.errors.size() == 1);
        CHECK(r.errors[0].find("aliases.json") != std::string::npos);
        CHECK_FALSE(r.userFileLoaded);
        CHECK(f.reg.find("L")->name() == "LINE");  // back to the default
    }
}

TEST_CASE("alias entries pointing nowhere or shadowing commands are reported and skipped") {
    Fx f;
    AliasReport r = f.reg.setUserAliases({{"Q", "NOPE"}, {"LINE", "CIRCLE"}, {"OK", "LINE"}});
    CHECK(r.errors.size() == 2);
    CHECK(f.reg.find("Q") == nullptr);
    CHECK(f.reg.find("LINE")->name() == "LINE");
    CHECK(f.reg.find("OK")->name() == "LINE");
}

TEST_CASE("a missing alias file is not an error") {
    Fx f;
    AliasReport r = f.reg.loadUserAliases(tempDir("missing") / "aliases.json");
    CHECK(r.errors.empty());
    CHECK_FALSE(r.userFileLoaded);
}

TEST_CASE("JSON strings: escapes, unicode, BOM") {
    auto r = parseAliasJson("\xEF\xBB\xBF{ \"A\\u0042\" : \"L\\\"x\", \"\\u0e01\": \"v\" }");
    REQUIRE(r);
    CHECK(r.value().at("AB") == "L\"x");
    CHECK(r.value().at("\xE0\xB8\x81") == "v");
    CHECK(parseAliasJson("{}").ok());
    CHECK_FALSE(parseAliasJson("{\"a\":\"b\",}").ok());
}

TEST_CASE("ALIASES command reloads the file from the settings folder") {
    Document doc;
    Registry reg;
    registerBuiltinCommands(reg);
    REQUIRE(reg.add(std::make_unique<Named>("LINE", std::vector<std::string>{"L"})));
    ScriptHost host(doc, reg);
    auto dir = tempDir("cmd");
    host.setSettingsFolder(dir);

    host.run("ALIASES", {});
    CHECK(host.logContains("0 user alias"));

    write(dir / "aliases.json", R"({"LL": "LINE"})");
    host.run("ALIASES", {});
    CHECK(host.logContains("1 user alias"));
    CHECK(reg.find("LL")->name() == "LINE");

    write(dir / "aliases.json", "{oops");
    host.clearLog();
    host.run("ALIASES", {});
    CHECK(host.logContains("Alias error"));
    CHECK(reg.find("LL") == nullptr);
    CHECK(reg.find("L")->name() == "LINE");
}

TEST_CASE("an acad.pgp next to aliases.json adds aliases, json wins, odd lines are warnings") {
    Fx f;
    auto dir = tempDir("pgp");
    write(dir / "acad.pgp",
          "; my aliases\n[Command]\nLL, *LINE\nCCC, *circle\nL, *CIRCLE\nXX, *NOSUCH\n"
          "LINE, *LINE\nC, *CIRCLE\nBAD\n[External Commands]\nNOTEPAD, START NOTEPAD, 1,\n");
    // The pgp alone, no aliases.json.
    AliasReport r = f.reg.loadUserAliases(dir / "aliases.json");
    CHECK(r.errors.empty());
    CHECK(f.reg.find("LL")->name() == "LINE");
    CHECK(f.reg.find("ccc")->name() == "CIRCLE");
    CHECK(f.reg.find("L")->name() == "CIRCLE");  // a pgp line may override a built-in alias
    CHECK(f.reg.find("XX") == nullptr);
    // "LINE, *LINE" (a command name) and "C, *CIRCLE" (already so) are silent; the rest warn.
    std::string all;
    for (const auto& w : r.warnings)
        all += w + "\n";
    CHECK(all.find("XX -> NOSUCH") != std::string::npos);
    CHECK(all.find("no comma") != std::string::npos);
    CHECK(all.find("C -> CIRCLE") == std::string::npos);
    CHECK(all.find("LINE -> LINE") == std::string::npos);
    // A json entry for the same alias wins.
    write(dir / "aliases.json", R"({"LL": "CIRCLE"})");
    r = f.reg.loadUserAliases(dir / "aliases.json");
    CHECK(r.userFileLoaded);
    CHECK(f.reg.find("LL")->name() == "CIRCLE");
    CHECK(f.reg.find("CCC")->name() == "CIRCLE");
}
