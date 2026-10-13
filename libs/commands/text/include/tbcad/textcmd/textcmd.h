#pragma once

#include "tbcad/cmd/registry.h"

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

}  // namespace tbcad::textcmd
