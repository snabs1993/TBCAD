#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "tbcad/cmd/registry.h"
#include "tbcad/platform/keybindings.h"

namespace tbcad::ui {

// Model behind the Aliases page of the Settings dialog: every edit is validated by the registry
// and written to aliases.json at once, so users never touch the JSON.
class AliasEditor {
public:
    AliasEditor(cmd::Registry& registry, std::filesystem::path file)
        : registry_(registry), file_(std::move(file)) {}

    // Alias table sorted by alias: built-in and user rows, conflicts marked.
    std::vector<cmd::AliasEntry> rows() const { return registry_.aliasTable(); }

    // Adds or changes a user alias. Returns an error text; empty means done (and saved).
    std::string set(const std::string& alias, const std::string& command);
    // Removes a user alias (a built-in one comes back, if there was one). Empty = done.
    std::string remove(const std::string& alias);

private:
    std::string commit(std::map<std::string, std::string> next);
    cmd::Registry& registry_;
    std::filesystem::path file_;
};

struct KeyRow {
    std::string action{};
    std::string chord{};  // "Ctrl+Z", empty = unbound
    std::string defaultChord{};
    bool overridden = false;
    std::string conflictWith{};  // another action on the same chord, if any
};

// Model behind the Keybindings page. Edits go to keybindings.json at once.
class KeybindingEditor {
public:
    KeybindingEditor(platform::Keybindings& keys, std::filesystem::path file)
        : keys_(keys), file_(std::move(file)) {}

    std::vector<KeyRow> rows() const;

    struct Outcome {
        bool ok = false;
        std::string error{};
        // The action that already uses the chord; nothing changed. Call again with
        // `takeOver` to unbind it and use the chord here.
        std::string conflictWith{};
    };
    // `chordText` is a chord such as "Ctrl+Shift+K" (what the capture box produced); empty
    // unbinds the action.
    Outcome set(const std::string& action, const std::string& chordText, bool takeOver = false);
    // Back to the chord declared in code.
    Outcome reset(const std::string& action);

private:
    Outcome save();
    platform::Keybindings& keys_;
    std::filesystem::path file_;
};

}  // namespace tbcad::ui
