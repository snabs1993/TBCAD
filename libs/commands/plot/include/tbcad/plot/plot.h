#pragma once

#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "tbcad/cmd/host.h"
#include "tbcad/cmd/registry.h"
#include "tbcad/core/core.h"
#include "tbcad/export/plot.h"
#include "tbcad/layout/pagesetup.h"
#include "tbcad/plotstyle/plotstyle.h"
#include "tbcad/print/print.h"
#include "tbcad/ui/ui_registry.h"

namespace tbcad::plot {

// Called by the generated tbcad::registerAllCommands / registerAllUi (see module.cmake).
void registerCommands(cmd::Registry& registry);
void registerUi(ui::UiRegistry& registry);

enum class Format { Pdf, Svg, Png, Jpeg };
// File extension of the format with the dot (".pdf").
const char* extension(Format format);
const char* formatName(Format format);  // "PDF", "SVG", "PNG", "JPEG"
// Resolution of PNG / JPEG output in dpi (setting "plot.dpi", 10..2400, default 300) and the JPEG
// quality (setting "plot.jpegQuality", 1..100, default 90).
double plotDpi(const cmd::IHost& host);
void setPlotDpi(cmd::IHost& host, double dpi);
int jpegQuality(const cmd::IHost& host);

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
/// Where a named style table is looked for after the built-ins: the drawing's folder (and a
/// "Plot Styles" folder in it), TBCAD's own plot styles folder, then AutoCAD's Plotters\Plot Styles
/// folders of the current user.
// Folders the user added (setting "plot.stylepaths", ";"- or newline-separated).
std::vector<std::filesystem::path> userStyleFolders(const cmd::IHost& host);
// Order: the drawing's folder, TBCAD's plot styles, the user's folders, AutoCAD's, Downloads and
// Documents.
std::vector<std::filesystem::path> styleSearchFolders(const cmd::IHost& host);
/// True when the chosen style table is named but cannot be found (plotting would silently use
/// none).
bool styleTableMissing(const cmd::IHost& host);
Result<std::shared_ptr<const plotstyle::PlotStyleTable>> resolveStyleTable(const cmd::IHost& host,
                                                                           const std::string& name);
// The file a named table is read from; empty for built-in tables, "none" and tables not found.
std::filesystem::path styleTableFile(const cmd::IHost& host, const std::string& name);
// Built-in names, then the files of the user folder.
std::vector<std::string> listStyleTables(const cmd::IHost& host);

// The print job the "plot.printer" / "plot.copies" settings describe (PRINTER command), titled
// after the open drawing.
print::PrintJob printJob(const cmd::IHost& host, bool showDialog);

// ---- page setups (named plot options saved in the drawing, see layout/pagesetup.h)
// ----------------

// Finds style tables for settingsFor / plotSheetList through resolveStyleTable.
layout::StyleResolver styleResolver(const cmd::IHost& host);
// The current plot settings (host settings and style table) as a named page setup.
layout::PageSetup currentPageSetup(const cmd::IHost& host, const std::string& name);
// Makes the setup the current plot settings (settings, style table choice).
void applyPageSetup(cmd::IHost& host, const layout::PageSetup& setup);
// When the layout chosen for plotting (or model space) has a page setup assigned, applies it and
// returns a line for the command line; empty otherwise.
std::string applyAssignedPageSetup(const Document& doc, cmd::IHost& host);
// applyAssignedPageSetup the first time the layout chosen for plotting is plotted (in this
// drawing), so PLOT on a layout tab starts from that layout's own page setup.
std::string followPlottedLayoutSetup(const Document& doc, cmd::IHost& host);

// Adds the extension for the format when the path has none.
std::string withExtension(std::string path, Format format);
// Like withExtension, but a path that ends in another export extension (.pdf .svg .png .jpg
// .jpeg) gets the extension of the format instead.
std::string forFormat(std::string path, Format format);
// The visible drawing area for PlotArea::Display, from the host's view.
BBox2 viewWindow(const cmd::IHost& host);
// "Fit", "1:100", "100" -> settings (fit / scaleN). False when the text is not a scale.
bool parseScale(const std::string& text, exportlib::PlotSettings& settings);
std::string scaleText(const exportlib::PlotSettings& settings);

// The sheet the plot settings describe: the chosen layout, or model space. Display areas are
// resolved from the host's view.
// Runs fn; with TBCAD_PLOT_TIMING set, writes "plot timing: <name> <ms>" to stderr.
void timePhase(const char* name, const std::function<void()>& fn);

Result<exportlib::PlotSheet> buildPlotSheet(const Document& doc, const cmd::IHost& host,
                                            exportlib::PlotSettings settings);

// Plot stamp: a line of text (file name, layout, date, scale, paper) in the lower-left corner of
// the printable area, plotted with the sheet. Settings "plot.stamp" (0/1) and "plot.stampText".
// Tokens: %file% %layout% %date% %time% %scale% %paper%.
bool plotStampOn(const cmd::IHost& host);
void setPlotStamp(cmd::IHost& host, bool on);
std::string plotStampText(const cmd::IHost& host);
void setPlotStampText(cmd::IHost& host, const std::string& text);
std::string expandStamp(const std::string& tmpl, const cmd::IHost& host,
                        const exportlib::PlotSheet& sheet, const exportlib::PlotSettings& settings);
// The same for a sheet of another drawing (PUBLISH): %file% and %layout% come from the arguments.
std::string expandStamp(const std::string& tmpl, const std::string& drawingFile,
                        const std::string& layoutName, const exportlib::PlotSheet& sheet,
                        const exportlib::PlotSettings& settings);
// Adds the stamp to the sheet when it is switched on.
void addPlotStamp(exportlib::PlotSheet& sheet, const cmd::IHost& host,
                  const exportlib::PlotSettings& settings);
void addPlotStamp(exportlib::PlotSheet& sheet, const cmd::IHost& host,
                  const exportlib::PlotSettings& settings, const std::string& drawingFile,
                  const std::string& layoutName);

// Reads a .tbcad or .dxf drawing (sheet lists, the PUBLISH dialog).
Result<std::unique_ptr<Document>> readDrawingFile(const std::filesystem::path& file);

// ETRANSMIT: the files to send with a saved drawing. `target` is the path inside the package
// ('/'-separated): xrefs and underlays keep their path relative to the drawing that refers to them
// (nested xrefs included), files from elsewhere go to "refs/"; fonts the text styles name go to
// "fonts/", plot style tables of the plot settings and page setups to "plotstyles/".
struct TransmitFile {
    std::filesystem::path source{};
    std::string target{};
    std::string kind{};  // "drawing", "xref", "image", "pdf", "font", "plot style"
    std::string note{};  // what uses it; "moved" notes for files that need a new path
    bool moved = false;
};
struct TransmitSet {
    std::vector<TransmitFile> files{};
    std::vector<std::string> missing{};
    std::vector<std::string> notes{};
};
TransmitSet transmittalSet(const Document& doc, const std::filesystem::path& drawingFile,
                           const cmd::IHost& host);
// The transmittal.txt text.
std::string transmittalReport(const TransmitSet& set, const std::filesystem::path& drawingFile);

// PDF layers: with "plot.pdfLayers" on (the default) a PDF lists the drawing's layers so a viewer
// can switch them; off writes a flat PDF.
bool pdfLayersOn(const cmd::IHost& host);
void setPdfLayers(cmd::IHost& host, bool on);

// Builds the sheet from the settings (Display area is resolved from the host's view) and writes the
// file. The returned text is the one-line summary for the command line.
Result<std::string> exportFile(const Document& doc, const cmd::IHost& host,
                               exportlib::PlotSettings settings, Format format,
                               const std::string& path);

// ---- plot frames (named model space sheets, see layout/plotframes.h) --------------------------
// One sheet per named frame, in the order given, with the current plot options (or each frame's
// page setup), the plot stamp, upside down and PDF layer choices applied.
Result<std::vector<exportlib::PlotSheet>> buildFrameSheets(const Document& doc,
                                                           const cmd::IHost& host,
                                                           const std::vector<std::string>& names);
// Plots the frames into one multi-page PDF (`file` "Printer" / "Dialog": to the printer). The
// returned text is the summary for the command line.
Result<std::string> plotFramesTo(const Document& doc, const cmd::IHost& host,
                                 const std::vector<std::string>& names, const std::string& file);

}  // namespace tbcad::plot
