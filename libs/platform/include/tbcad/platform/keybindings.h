#pragma once

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "tbcad/core/result.h"

namespace tbcad::platform {

// A key chord such as "Ctrl+Shift+Z". `key` is upper-case: a letter, a digit, F1..F24 or a name
// such as ESCAPE, ENTER, DELETE, TAB, SPACE, HOME, END, LEFT.
struct KeyChord {
    bool ctrl = false;
    bool shift = false;
    bool alt = false;
    std::string key{};

    std::string toString() const;
    friend bool operator==(const KeyChord&, const KeyChord&) = default;
};

// Case-insensitive; modifiers in any order; "Control" and "Ctrl" are the same.
std::optional<KeyChord> parseChord(std::string_view text);

// Action id -> key chord. Whoever owns an action declares its default in code; keybindings.json
// (an object of action id to chord text, "" = unbound) overrides it.
class Keybindings {
public:
    void declare(std::string actionId, std::string defaultChord);
    // Returns warnings (bad chord text, unknown action ids, two actions on one chord). A missing
    // file is not an error.
    Result<std::vector<std::string>> load(const std::filesystem::path& file);
    Result<void> save(const std::filesystem::path& file) const;

    std::optional<KeyChord> chordOf(std::string_view actionId) const;
    // The action bound to this chord, or empty.
    std::string actionFor(const KeyChord& chord) const;
    // Text for menus ("Ctrl+Z"), empty when unbound.
    std::string label(std::string_view actionId) const;

    void set(std::string_view actionId, std::string chord);
    // The chord declared in code, and whether keybindings.json (or set()) changed it.
    std::string defaultChordOf(std::string_view actionId) const;
    bool overridden(std::string_view actionId) const;
    // Removes the override so the declared chord applies again.
    void reset(std::string_view actionId);
    std::vector<std::string> actions() const;

private:
    std::map<std::string, std::string> defaults_{};
    std::map<std::string, std::string> overrides_{};
};

}  // namespace tbcad::platform
