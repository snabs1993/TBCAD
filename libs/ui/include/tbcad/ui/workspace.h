#pragma once

// Workspaces: named window layouts. A workspace remembers which panels are shown and, for the ones
// the user saved, the docking layout. They live as <name>.json files in the "workspaces" folder of
// the settings folder; three built-in ones (Drafting, Annotation, Minimal) are always offered and
// cannot be changed or deleted.

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace tbcad::ui {

struct Workspace {
    std::string name{};
    std::map<std::string, bool> panels{};  // UiRegistry panel id -> shown
    bool commandLine = true;
    bool properties = true;
    // Dear ImGui docking layout (its ini text). Empty = use the default layout.
    std::string dockIni{};
    bool builtin = false;
};

class WorkspaceStore {
public:
    explicit WorkspaceStore(std::filesystem::path folder) : folder_(std::move(folder)) {}

    // Built-in workspaces first, then the user's in name order.
    std::vector<std::string> names() const;
    bool isBuiltin(const std::string& name) const;
    std::optional<Workspace> load(const std::string& name) const;
    // Saves a user workspace (replacing one of the same name). Returns an error text, empty on
    // success. Built-in names are refused.
    std::string save(const Workspace& ws);
    std::string remove(const std::string& name);
    std::string rename(const std::string& from, const std::string& to);

    // The file name a workspace is stored under: the name with characters that file systems do not
    // allow replaced by '_'.
    static std::string fileStem(const std::string& name);
    static std::vector<Workspace> builtins();

private:
    std::filesystem::path fileOf(const std::string& name) const;
    std::filesystem::path folder_;
};

}  // namespace tbcad::ui
