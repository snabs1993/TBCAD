#pragma once

#include <string>

#include "tbcad/cmd/host.h"
#include "tbcad/cmd/registry.h"
#include "tbcad/core/core.h"
#include "tbcad/export/plot.h"
#include "tbcad/ui/ui_registry.h"

namespace tbcad::plot {

// Called by the generated tbcad::registerAllCommands / registerAllUi (see module.cmake).
void registerCommands(cmd::Registry& registry);
void registerUi(ui::UiRegistry& registry);

enum class Format { Pdf, Svg };

// The plot settings live in the host's free-form settings ("plot.*" keys), so the PLOT command, the
// dialog panel and the next session all see the same values.
exportlib::PlotSettings loadSettings(const cmd::IHost& host);
void saveSettings(cmd::IHost& host, const exportlib::PlotSettings& settings);
// Output file used last (or the default name for the format).
std::string lastFile(const cmd::IHost& host, Format format);
// Default output name: next to the open drawing with the format's extension; otherwise the file
// used last.
std::string defaultFile(const cmd::IHost& host, Format format);
void setLastFile(cmd::IHost& host, Format format, const std::string& path);

// Adds the extension for the format when the path has none.
std::string withExtension(std::string path, Format format);
// The visible drawing area for PlotArea::Display, from the host's view.
BBox2 viewWindow(const cmd::IHost& host);
// "Fit", "1:100", "100" -> settings (fit / scaleN). False when the text is not a scale.
bool parseScale(const std::string& text, exportlib::PlotSettings& settings);
std::string scaleText(const exportlib::PlotSettings& settings);

// Builds the sheet from the settings (Display area is resolved from the host's view) and writes the
// file. The returned text is the one-line summary for the command line.
Result<std::string> exportFile(const Document& doc, const cmd::IHost& host,
                               exportlib::PlotSettings settings, Format format,
                               const std::string& path);

}  // namespace tbcad::plot
