#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "tbcad/cmd/host.h"
#include "tbcad/cmd/registry.h"
#include "tbcad/core/core.h"
#include "tbcad/export/plot.h"
#include "tbcad/plotstyle/plotstyle.h"
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

// The layout to plot (host setting "plot.layout"); empty plots model space.
std::string plotLayout(const cmd::IHost& host);
void setPlotLayout(cmd::IHost& host, const std::string& name);

// Plot style table (setting "plot.style"): a built-in name ("monochrome.ctb" ...), a file in the
// user "plot styles" folder, a full path to a .tbstyle / .ctb file, or "none" to plot as drawn. The
// default is monochrome.ctb.
std::string plotStyleName(const cmd::IHost& host);
void setPlotStyleName(cmd::IHost& host, const std::string& name);
// The user's folder for plot style tables (empty when the host has no settings folder).
std::filesystem::path plotStyleFolder(const cmd::IHost& host);
// Finds and loads a table by name. A null table with ok() means "none".
Result<std::shared_ptr<const plotstyle::PlotStyleTable>> resolveStyleTable(const cmd::IHost& host,
                                                                           const std::string& name);
// Built-in names, then the files of the user folder.
std::vector<std::string> listStyleTables(const cmd::IHost& host);

// Adds the extension for the format when the path has none.
std::string withExtension(std::string path, Format format);
// The visible drawing area for PlotArea::Display, from the host's view.
BBox2 viewWindow(const cmd::IHost& host);
// "Fit", "1:100", "100" -> settings (fit / scaleN). False when the text is not a scale.
bool parseScale(const std::string& text, exportlib::PlotSettings& settings);
std::string scaleText(const exportlib::PlotSettings& settings);

// The sheet the plot settings describe: the chosen layout, or model space. Display areas are
// resolved from the host's view.
Result<exportlib::PlotSheet> buildPlotSheet(const Document& doc, const cmd::IHost& host,
                                            exportlib::PlotSettings settings);

// Builds the sheet from the settings (Display area is resolved from the host's view) and writes the
// file. The returned text is the one-line summary for the command line.
Result<std::string> exportFile(const Document& doc, const cmd::IHost& host,
                               exportlib::PlotSettings settings, Format format,
                               const std::string& path);

}  // namespace tbcad::plot
