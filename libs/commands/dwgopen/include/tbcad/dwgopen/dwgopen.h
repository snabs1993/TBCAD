// Registers the DWG (read-only) reader and the DXF writer with the application core.
#pragma once

#include <filesystem>
#include <vector>

#include "tbcad/ui/app_core.h"

namespace tbcad::dwgopen {

// Reads a .dwg file (never modifies it). Messages: a summary line, then the warnings.
Result<ui::ForeignOpenResult> readDwgFile(const std::filesystem::path& path);

// ".dwg" opens read-only through readDwgFile (Save As .tbcad/.dxf is handled by AppCore).
void registerForeignFormats(ui::AppCore& core);

}  // namespace tbcad::dwgopen
