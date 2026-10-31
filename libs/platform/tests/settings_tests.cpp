#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>

#include "tbcad/platform/settings.h"

using namespace tbcad::platform;
namespace fs = std::filesystem;

namespace {
fs::path tempDir(const char* name) {
    fs::path d = fs::temp_directory_path() / (std::string("tbcad_platform_") + name);
    fs::remove_all(d);
    fs::create_directories(d);
    return d;
}
}  // namespace

TEST_CASE("settings defaults, typed access and persistence") {
    Settings s = Settings::withDefaults();
    CHECK(s.getString("ui.theme") == "dark");
    CHECK(s.getNumber("autosave.minutes") == 10);
    CHECK(s.getBool("autosave.enabled"));
    CHECK(s.getNumber("no.such.key", 7) == 7);

    s.setString("ui.theme", "light");
    s.setNumber("autosave.minutes", 3);
    CHECK(s.dirty());

    fs::path dir = tempDir("settings");
    REQUIRE(s.save(dir / "sub" / "settings.json").ok());
    CHECK_FALSE(s.dirty());
    CHECK_FALSE(fs::exists(dir / "sub" / "settings.json.tmp"));

    Settings loaded = Settings::withDefaults();
    REQUIRE(loaded.load(dir / "sub" / "settings.json").ok());
    CHECK(loaded.getString("ui.theme") == "light");
    CHECK(loaded.getNumber("autosave.minutes") == 3);
}

TEST_CASE("settings: missing file is fine, broken file reports and keeps defaults") {
    fs::path dir = tempDir("settings_bad");
    Settings s = Settings::withDefaults();
    CHECK(s.load(dir / "none.json").ok());
    { std::ofstream(dir / "bad.json") << "{ not json"; }
    auto r = s.load(dir / "bad.json");
    REQUIRE_FALSE(r.ok());
    CHECK(r.error().message.find("bad.json") != std::string::npos);
    CHECK(s.getString("ui.theme") == "dark");
}

TEST_CASE("settings keep unknown keys when saving") {
    fs::path dir = tempDir("settings_unknown");
    { std::ofstream(dir / "s.json") << R"({"future.option": 42, "ui.theme": "light"})"; }
    Settings s;
    REQUIRE(s.load(dir / "s.json").ok());
    REQUIRE(s.save(dir / "t.json").ok());
    Settings t;
    REQUIRE(t.load(dir / "t.json").ok());
    CHECK(t.getNumber("future.option") == 42);
}

TEST_CASE("recent files: most recent first, no duplicates, capped") {
    Settings s;
    for (int i = 0; i < 15; ++i)
        s.addRecentFile("f" + std::to_string(i) + ".tbcad");
    s.addRecentFile("f10.tbcad");
    auto r = s.recentFiles();
    REQUIRE(r.size() == Settings::kMaxRecent);
    CHECK(r[0] == "f10.tbcad");
    CHECK(r[1] == "f14.tbcad");
    int count = 0;
    for (const auto& x : r)
        count += x == "f10.tbcad";
    CHECK(count == 1);
    s.removeRecentFile("f10.tbcad");
    CHECK(s.recentFiles()[0] == "f14.tbcad");
}

TEST_CASE("resetToDefaults restores the dialog's options and keeps recent files") {
    Settings s = Settings::withDefaults();
    s.setString("ui.theme", "light");
    s.setNumber("ui.scale", 1.5);
    s.setString("view.crosshair", "off");
    s.setBool("hud.enabled", false);
    s.addRecentFile("a.tbcad");
    s.setString("custom.thing", "keep");
    s.resetToDefaults();
    CHECK(s.getString("ui.theme") == "dark");
    CHECK(s.getNumber("ui.scale") == 1.0);
    CHECK_FALSE(s.has("view.crosshair"));
    CHECK_FALSE(s.has("hud.enabled"));
    CHECK(s.recentFiles().size() == 1);
    CHECK(s.getString("custom.thing") == "keep");
    CHECK(s.dirty());
}

TEST_CASE("test.13: resetToDefaults also clears the plot style search folders of the Files page") {
    Settings s = Settings::withDefaults();
    s.setString("plot.stylepaths", "D:/tb/shots/ctb");
    s.resetToDefaults();
    CHECK(s.getString("plot.stylepaths", "") == "");
}

TEST_CASE("repair puts back values a hand-edited file made unusable") {
    fs::path d = tempDir("repair");
    {
        std::ofstream(d / "settings.json")
            << R"({"ui.scale": 0, "pick.aperturePx": "big", "drafting.osnap": "yes",
                  "autosave.minutes": 100000, "ui.theme": 5, "my.own": [1, 2]})";
    }
    Settings s = Settings::withDefaults();
    REQUIRE(s.load(d / "settings.json"));
    const auto fixed = s.repair();
    CHECK(fixed.size() == 5);
    CHECK(s.getNumber("ui.scale") == 0.75);
    CHECK(s.getNumber("pick.aperturePx") == 6);
    CHECK(s.getBool("drafting.osnap"));
    CHECK(s.getNumber("autosave.minutes") == 600);  // SAVETIME goes up to 600
    CHECK(s.getString("ui.theme") == "dark");
    CHECK(s.kindOf("my.own") == Settings::Kind::Other);  // not ours: left alone
    CHECK(s.dirty());

    Settings fresh = Settings::withDefaults();
    CHECK(fresh.repair().empty());
    CHECK_FALSE(fresh.dirty());
}
