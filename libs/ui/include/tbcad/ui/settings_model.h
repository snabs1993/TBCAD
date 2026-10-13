#pragma once

#include <filesystem>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "tbcad/cmd/registry.h"
#include "tbcad/platform/keybindings.h"

namespace tbcad::ui {

// An AutoCAD acad.pgp style alias file: `L, *LINE` lines, `;` comments, an [External Commands]
// section that is ignored. `skipped` explains every line that was not an alias.
struct PgpParse {
    std::map<std::string, std::string> aliases{};  // upper-case alias -> upper-case command
    std::vector<std::string> skipped{};
};
PgpParse parsePgp(std::string_view text);
// The text of a .pgp file holding `aliases`, sorted by alias, with a short header comment.
std::string writePgp(const std::map<std::string, std::string>& aliases);

struct AliasImportReport {
    std::size_t added = 0;               // new or changed user aliases
    std::vector<std::string> skipped{};  // lines/aliases not imported, each with the reason
    std::string error{};                 // set when nothing was imported at all
};

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

    // Reads a .pgp style file's text. Aliases whose command does not exist here, or that are
    // already in effect, are skipped and listed; the rest are added to the user aliases in one
    // step and saved.
    AliasImportReport importPgp(std::string_view text);
    // The aliases in effect (built-in plus user) as .pgp text.
    std::string exportPgp() const;

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

    // Key binding profiles: the keybindings.json format. importFrom checks the file first and
    // changes nothing when it cannot be read; `warnings` gets problems found in it.
    std::string exportTo(const std::filesystem::path& file) const;
    Outcome importFrom(const std::filesystem::path& file, std::vector<std::string>* warnings);

private:
    Outcome save();
    platform::Keybindings& keys_;
    std::filesystem::path file_;
};

}  // namespace tbcad::ui
