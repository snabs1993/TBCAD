#pragma once

#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "tbcad/cmd/command.h"

namespace tbcad::cmd {

// Problems found while building the alias table or reading aliases.json. Never fatal: the
// registry always ends up with a usable table.
struct AliasReport {
    std::vector<std::string> errors{};    // unreadable file, bad JSON, unknown target ...
    std::vector<std::string> warnings{};  // conflicts between built-in aliases
    bool userFileLoaded = false;
    std::size_t userAliases = 0;
    bool clean() const { return errors.empty() && warnings.empty(); }
};

class Registry {
public:
    Registry();
    ~Registry();
    Registry(const Registry&) = delete;
    Registry& operator=(const Registry&) = delete;

    // Names are upper-case ASCII letters, digits, '_' and '-' (a leading '-' is allowed, as in
    // "-LAYER"). Fails on a bad or duplicate name.
    Result<void> add(std::unique_ptr<Command> command);

    // By command name or alias, case-insensitive. Null when unknown.
    Command* find(std::string_view nameOrAlias) const;
    // Commands sorted by name.
    std::vector<Command*> list() const;

    // Effective alias -> command name table (upper case).
    const std::map<std::string, std::string>& aliases() const { return aliases_; }
    std::vector<std::string> aliasesOf(std::string_view commandName) const;
    // Conflicts among built-in aliases found by the last rebuild.
    const std::vector<std::string>& conflicts() const { return conflicts_; }

    // Reads a user alias file ({"L": "LINE", "CC": "CIRCLE"}) over the built-in aliases. A
    // missing file is fine (defaults only). A broken file produces an error in the report and
    // leaves the defaults in force. A relative-less empty path just resets to the defaults.
    AliasReport loadUserAliases(const std::filesystem::path& file);
    // Same with the table already parsed (tests, settings UI).
    AliasReport setUserAliases(const std::map<std::string, std::string>& userAliases);

private:
    AliasReport rebuild();

    std::map<std::string, std::unique_ptr<Command>> commands_{};
    std::map<std::string, std::string> user_{};  // alias -> command, as given by the user
    std::map<std::string, std::string> aliases_{};
    std::vector<std::string> conflicts_{};
};

// Commands that belong to the framework itself: UNDO [U], REDO, UNDOBEGIN, UNDOEND, ALIASES.
void registerBuiltinCommands(Registry& registry);

// Parses the alias file text into alias -> command pairs. Exposed for tests.
Result<std::map<std::string, std::string>> parseAliasJson(std::string_view text);

}  // namespace tbcad::cmd
