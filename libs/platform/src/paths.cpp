#include "tbcad/platform/paths.h"

#include <cstdlib>
#include <string>
#include <system_error>

namespace tbcad::platform {

std::optional<std::string> processEnvironment(std::string_view name) {
    std::string key(name);
#ifdef _WIN32
    char* value = nullptr;
    std::size_t len = 0;
    if (_dupenv_s(&value, &len, key.c_str()) != 0 || value == nullptr)
        return std::nullopt;
    std::string result(value);
    std::free(value);
    if (result.empty())
        return std::nullopt;
    return result;
#else
    const char* value = std::getenv(key.c_str());
    if (value == nullptr || *value == '\0')
        return std::nullopt;
    return std::string(value);
#endif
}

AppPaths defaultAppPaths(EnvLookup lookup, bool windows) {
    AppPaths p;
    auto cfgOverride = lookup("TBCAD_CONFIG_DIR");
    auto dataOverride = lookup("TBCAD_DATA_DIR");
    if (windows) {
        if (auto v = lookup("APPDATA"))
            p.configDir = std::filesystem::path(*v) / "TBCAD";
        if (auto v = lookup("LOCALAPPDATA"))
            p.dataDir = std::filesystem::path(*v) / "TBCAD";
    } else {
        auto home = lookup("HOME");
        if (auto v = lookup("XDG_CONFIG_HOME"))
            p.configDir = std::filesystem::path(*v) / "tbcad";
        else if (home)
            p.configDir = std::filesystem::path(*home) / ".config" / "tbcad";
        if (auto v = lookup("XDG_DATA_HOME"))
            p.dataDir = std::filesystem::path(*v) / "tbcad";
        else if (home)
            p.dataDir = std::filesystem::path(*home) / ".local" / "share" / "tbcad";
    }
    if (cfgOverride)
        p.configDir = *cfgOverride;
    if (dataOverride)
        p.dataDir = *dataOverride;
    // Without any usable base the files go next to the program's working directory.
    if (p.configDir.empty())
        p.configDir = std::filesystem::path("TBCAD-config");
    if (p.dataDir.empty())
        p.dataDir = std::filesystem::path("TBCAD-data");
    return p;
}

std::filesystem::path longPath(const std::filesystem::path& p, bool windows) {
    if (!windows || p.empty())
        return p;
    const std::wstring text = p.generic_wstring();
    // Windows only applies the limit to paths of 260 characters and more; leave the rest alone.
    if (text.size() < 240 || text.rfind(L"//?/", 0) == 0 || text.rfind(L"//./", 0) == 0)
        return p;
    std::filesystem::path abs = p;
    std::error_code ec;
    if (!abs.is_absolute()) {
        abs = std::filesystem::absolute(p, ec);
        if (ec)
            return p;
    }
    std::wstring w = abs.lexically_normal().generic_wstring();
    for (wchar_t& c : w)
        if (c == L'/')
            c = L'\\';
    if (w.rfind(L"\\\\", 0) == 0)  // \\server\share\...
        return std::filesystem::path(L"\\\\?\\UNC\\" + w.substr(2));
    return std::filesystem::path(L"\\\\?\\" + w);
}

bool ensureDirectory(const std::filesystem::path& dir) {
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    return !ec && std::filesystem::is_directory(dir, ec);
}

}  // namespace tbcad::platform
