#include <doctest/doctest.h>

#include <map>

#include "tbcad/platform/paths.h"

using namespace tbcad::platform;

namespace {
std::map<std::string, std::string>& fakeEnv() {
    static std::map<std::string, std::string> env;
    return env;
}
std::optional<std::string> fake(std::string_view name) {
    auto it = fakeEnv().find(std::string(name));
    if (it == fakeEnv().end())
        return std::nullopt;
    return it->second;
}
}  // namespace

TEST_CASE("windows paths use APPDATA and LOCALAPPDATA") {
    fakeEnv() = {{"APPDATA", "C:/Users/a/AppData/Roaming"},
                 {"LOCALAPPDATA", "C:/Users/a/AppData/Local"}};
    AppPaths p = defaultAppPaths(fake, true);
    CHECK(p.configDir == std::filesystem::path("C:/Users/a/AppData/Roaming/TBCAD"));
    CHECK(p.settingsFile().filename() == "settings.json");
    CHECK(p.keybindingsFile().filename() == "keybindings.json");
    CHECK(p.autosaveDir() == std::filesystem::path("C:/Users/a/AppData/Local/TBCAD/autosave"));
}

TEST_CASE("linux paths follow XDG with fallbacks") {
    fakeEnv() = {{"HOME", "/home/a"}};
    AppPaths p = defaultAppPaths(fake, false);
    CHECK(p.configDir == std::filesystem::path("/home/a/.config/tbcad"));
    CHECK(p.dataDir == std::filesystem::path("/home/a/.local/share/tbcad"));
    fakeEnv()["XDG_CONFIG_HOME"] = "/cfg";
    CHECK(defaultAppPaths(fake, false).configDir == std::filesystem::path("/cfg/tbcad"));
}

TEST_CASE("environment overrides win") {
    fakeEnv() = {{"TBCAD_CONFIG_DIR", "/portable/cfg"}, {"TBCAD_DATA_DIR", "/portable/data"}};
    AppPaths p = defaultAppPaths(fake, true);
    CHECK(p.configDir == std::filesystem::path("/portable/cfg"));
    CHECK(p.dataDir == std::filesystem::path("/portable/data"));
}

TEST_CASE("no environment still gives usable relative folders") {
    fakeEnv().clear();
    AppPaths p = defaultAppPaths(fake, true);
    CHECK_FALSE(p.configDir.empty());
    CHECK_FALSE(p.dataDir.empty());
}

TEST_CASE("long paths: only Windows paths of 240 characters or more get the prefix") {
    const std::filesystem::path shortPath("//server/share/a.dwg");
    CHECK(longPath(shortPath, true) == shortPath);
    const std::string longName(250, 'x');
    const std::filesystem::path longUnc("//server/share/" + longName + ".dwg");
    CHECK(longPath(longUnc, false) == longUnc);
    CHECK(longPath({}, true).empty());
#ifdef _WIN32
    // Path semantics of the other platforms differ (a double slash is not a share there).
    CHECK(longPath(longUnc, true) ==
          std::filesystem::path(L"\\\\?\\UNC\\server\\share\\" +
                                std::wstring(longName.begin(), longName.end()) + L".dwg"));
    const std::filesystem::path prefixed(L"\\\\?\\C:\\" +
                                         std::wstring(longName.begin(), longName.end()));
    CHECK(longPath(prefixed, true) == prefixed);
#endif
}
