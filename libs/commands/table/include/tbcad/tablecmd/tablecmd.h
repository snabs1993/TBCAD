#pragma once

#include <string>
#include <vector>

#include "tbcad/cmd/registry.h"

namespace tbcad::ui {
class UiRegistry;
}

namespace tbcad::tablecmd {

// TABLE [TB] and TABLEEDIT [TE]. Also registers the display generator of Table with geom and its
// explode hook with the blocks module's ExplodeRegistry.
void registerCommands(cmd::Registry& registry);

// TABLEEXPORT and TABLEIMPORT (CSV); registerCommands calls it.
void registerCsvCommands(cmd::Registry& registry);

// DATALINK and DATALINKUPDATE: a table linked to a CSV or XLSX file; registerCommands calls it.
void registerDataLinkCommands(cmd::Registry& registry);

// The rows of a CSV or XLSX file (the first sheet; numbers and shared strings as text). Empty and
// `error` set when the file cannot be read.
std::vector<std::vector<std::string>> readLinkRows(const std::string& file, std::string& error);

// Menu entries for TABLE and TABLEEDIT.
void registerUi(ui::UiRegistry& ui);

}  // namespace tbcad::tablecmd
