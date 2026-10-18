#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "tbcad/core/result.h"
#include "tbcad/platform/json.h"

namespace tbcad::platform {

// User settings stored as one JSON object (settings.json). Keys are dotted lower-case names such
// as "ui.theme". Unknown keys are kept and written back, so newer settings survive an older
// program. Defaults live in code (see Settings::withDefaults); the file only needs the changes.
class Settings {
public:
    Settings() = default;

    // The defaults of every setting the application itself defines.
    static Settings withDefaults();
    // Puts the options of the Settings dialog back to their defaults (not recent files,
    // workspaces or folders).
    void resetToDefaults();

    // Missing file: not an error (defaults stay). Broken file: error, defaults stay, and the
    // caller is expected to tell the user.
    Result<void> load(const std::filesystem::path& file);
    // Atomic: writes <file>.tmp, then renames over <file>.
    Result<void> save(const std::filesystem::path& file) const;

    bool getBool(std::string_view key, bool fallback = false) const;
    double getNumber(std::string_view key, double fallback = 0) const;
    std::string getString(std::string_view key, std::string_view fallback = {}) const;
    std::vector<std::string> getStrings(std::string_view key) const;

    void setBool(std::string_view key, bool v);
    void setNumber(std::string_view key, double v);
    void setString(std::string_view key, std::string v);
    void setStrings(std::string_view key, const std::vector<std::string>& v);
    bool has(std::string_view key) const;
    enum class Kind { Missing, Bool, Number, String, Other };
    Kind kindOf(std::string_view key) const;

    // Most recent first, at most kMaxRecent entries, no duplicates.
    static constexpr std::size_t kMaxRecent = 10;
    std::vector<std::string> recentFiles() const { return getStrings("files.recent"); }
    void addRecentFile(const std::string& path);
    void removeRecentFile(const std::string& path);

    bool dirty() const { return dirty_; }

private:
    Json root_{Json::Object{}};
    mutable bool dirty_ = false;
};

}  // namespace tbcad::platform
