#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "tbcad/cmd/registry.h"

namespace tbcad::ui {

// One row of the command bar's autocomplete list (and of the command palette).
struct CommandMatch {
    enum class Rank { ExactAlias, AliasPrefix, NamePrefix, Fuzzy };
    std::string name;         // LINE
    std::string alias;        // the alias that matched, or the first alias, or empty
    std::string description;  // one line
    Rank rank = Rank::Fuzzy;
};

// Commands matching `query`, best first: (1) exact alias or name, (2) alias prefix, (3) command
// name prefix, (4) fuzzy match on name and description. Ties keep alphabetical order. An empty
// query lists every command (the palette). At most `limit` rows. Matching is case-insensitive.
// Within one rank the commands used lately (`recent`, command names, latest first) come first, then
// the ones without a leading "-" (the command-line forms of dialogs; they lead only when the query
// starts with "-"), then shorter names.
std::vector<CommandMatch> matchCommands(const cmd::Registry& registry, std::string_view query,
                                        std::size_t limit = 12,
                                        const std::vector<std::string>& recent = {});

// Letters of `query` appear in `text` in order (case-insensitive).
bool isSubsequence(std::string_view query, std::string_view text);

// Length of the shortest prefix of keywords[index] that no other keyword shares; the letters
// the user has to type. At least 1.
std::size_t keywordKeyLength(const std::vector<std::string>& keywords, std::size_t index);

}  // namespace tbcad::ui
