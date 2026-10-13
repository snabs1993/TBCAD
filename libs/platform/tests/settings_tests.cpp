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
    {
        std::ofstream(dir / "bad.json") << "{ not json";
    }
    auto r = s.load(dir / "bad.json");
    REQUIRE_FALSE(r.ok());
    CHECK(r.error().message.find("bad.json") != std::string::npos);
    CHECK(s.getString("ui.theme") == "dark");
}

TEST_CASE("settings keep unknown keys when saving") {
    fs::path dir = tempDir("settings_unknown");
    {
        std::ofstream(dir / "s.json") << R"({"future.option": 42, "ui.theme": "light"})";
    }
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
