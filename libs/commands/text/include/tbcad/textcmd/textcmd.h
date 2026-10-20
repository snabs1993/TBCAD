#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "tbcad/cmd/registry.h"
#include "tbcad/core/document.h"

namespace tbcad::ui {
class UiRegistry;
}

namespace tbcad::textcmd {

// Called by the generated tbcad::registerAllCommands / registerAllUi (see module.cmake).
// registerCommands also registers the Text and MText display generators.
void registerCommands(cmd::Registry& registry);
void registerUi(ui::UiRegistry& registry);

// Conversions between stored MTEXT markup and what the editor shows: "\P" is a real newline in
// the editor, everything else (braces, other escapes) stays as written.
std::string markupToEditable(std::string_view markup);
std::string editableToMarkup(std::string_view editable);

// The words of MTEXT markup that SPELL would flag (English only; Thai and codes are skipped),
// with where each starts in `markup` and up to three suggestions.
struct SpellIssue {
    std::string word;
    std::size_t at = 0;
    std::vector<std::string> suggestions;
};
std::vector<SpellIssue> misspelledWords(const Document& doc, const std::string& markup);

}  // namespace tbcad::textcmd
