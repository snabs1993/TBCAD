#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace tbcad::cmd {

// Offline help pages for commands: usage, options and an example per command. Read from a
// Markdown-like text (docs/command-help.md, shipped as assets/help/commands.md):
//
//   ## LINE
//   Draws straight segments.
//   Usage: LINE, then pick the first point and each next point; Enter ends.
//   Options: Close, Undo
//   Example: LINE 0,0 100,0 @0,50 Enter
//
// A "##" line names the command; "Usage:", "Options:" and "Example:" start those fields; any
// other line is part of the summary.
struct HelpEntry {
    std::string name{};     // upper-case command name
    std::string summary{};  // free text (lines joined by spaces)
    std::string usage{};
    std::string options{};
    std::string example{};
};

class HelpCatalog {
public:
    // Adds the pages found in `text`; a later page of the same name replaces the earlier one.
    void parse(std::string_view text);
    bool empty() const { return entries_.empty(); }
    const std::vector<HelpEntry>& entries() const { return entries_; }
    // By command name (case-insensitive). Null when there is no page.
    const HelpEntry* find(std::string_view name) const;
    // Pages whose name or text contains every word of `query` (case-insensitive); name matches
    // first, then the rest, each sorted by name.
    std::vector<const HelpEntry*> search(std::string_view query) const;

private:
    std::vector<HelpEntry> entries_{};
};

// The page as plain lines for the command line / history.
std::vector<std::string> helpLines(const HelpEntry& entry);

}  // namespace tbcad::cmd
