#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace tbcad::platform {

// Where TBCAD keeps its files.
//   Windows: %APPDATA%\TBCAD (settings, keybindings, aliases) and %LOCALAPPDATA%\TBCAD (autosave)
//   Elsewhere: $XDG_CONFIG_HOME/tbcad (or ~/.config/tbcad) and $XDG_DATA_HOME/tbcad
// TBCAD_CONFIG_DIR and TBCAD_DATA_DIR override both (portable installs, tests).
struct AppPaths {
    std::filesystem::path configDir{};
    std::filesystem::path dataDir{};

    std::filesystem::path settingsFile() const { return configDir / "settings.json"; }
    std::filesystem::path keybindingsFile() const { return configDir / "keybindings.json"; }
    std::filesystem::path autosaveDir() const { return dataDir / "autosave"; }
    std::filesystem::path crashDir() const { return dataDir / "crashes"; }
};

// `lookup` returns an environment variable's value; defaults to the process environment. Taking
// it as a parameter keeps the logic testable.
using EnvLookup = std::optional<std::string> (*)(std::string_view name);
std::optional<std::string> processEnvironment(std::string_view name);

#ifdef _WIN32
inline constexpr bool kHostIsWindows = true;
#else
inline constexpr bool kHostIsWindows = false;
#endif

AppPaths defaultAppPaths(EnvLookup lookup = processEnvironment, bool windows = kHostIsWindows);

// File names travel as UTF-8 text (settings, recent files); these convert to and from the native
// path type without going through the ANSI code page.
inline std::filesystem::path pathFromUtf8(const std::string& text) {
    return std::filesystem::path(std::u8string(text.begin(), text.end()));
}
inline std::string pathToUtf8(const std::filesystem::path& p) {
    auto u = p.u8string();
    return std::string(u.begin(), u.end());
}

// Creates the directory (and parents). False on failure.
bool ensureDirectory(const std::filesystem::path& dir);

}  // namespace tbcad::platform
