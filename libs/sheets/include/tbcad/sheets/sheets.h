#pragma once

// Sheet lists: a text file naming the layouts to plot, in order, possibly from several drawings.
//
//   # comment
//   plans/ground.tbcad | A1 Plan
//   plans/roof.tbcad                  <- no layout: model space with the plot settings
//   plans/roof.tbcad | A1 Roof | Half size   <- plotted with the page setup "Half size"
//   plans/site.tbcad | | Site A3      <- model space with a page setup
//   plans/site.tbcad | | | Sheet 3    <- a model space plot frame (see layout/plotframes.h)
//
// Relative paths are relative to the list file. The PUBLISH panel saves its sheet lists (.tbdsd)
// in this format.

#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "tbcad/core/result.h"
#include "tbcad/export/plot.h"
#include "tbcad/layout/pagesetup.h"

namespace tbcad::sheets {

struct SheetEntry {
    std::filesystem::path file{};
    std::string layout{};  // empty: model space
    // A named page setup to plot with instead of the one the layout has: looked up in the sheet's
    // drawing, then in BuildOptions::setupSource. Empty: the layout's own setup.
    std::string pageSetup{};
    // A model space plot frame (layout empty): its window, paper and scale, with the page setup
    // above (or the frame's own) for the other options.
    std::string frame{};
    bool operator==(const SheetEntry&) const = default;
};

// Parses the list text; `baseDir` resolves relative file names. A bad line is an error naming it.
Result<std::vector<SheetEntry>> parseSheetList(const std::string& text,
                                               const std::filesystem::path& baseDir);
// Reads and parses a list file.
Result<std::vector<SheetEntry>> readSheetList(const std::filesystem::path& listFile);
// The list text for `entries`; files inside `baseDir` are written relative to it.
std::string formatSheetList(const std::vector<SheetEntry>& entries,
                            const std::filesystem::path& baseDir);
// Writes the list file (paths relative to its folder). Returns an error text, empty on success.
std::string writeSheetList(const std::vector<SheetEntry>& entries,
                           const std::filesystem::path& listFile);
// The sheets of one drawing: model space first, then each plot frame, then each layout in tab
// order.
std::vector<SheetEntry> sheetsOf(const Document& doc, const std::filesystem::path& file);

struct SheetSetReport {
    std::size_t pages = 0;
    std::vector<std::string> warnings{};  // sheets that overflow their paper
};

struct SheetSet {
    std::vector<exportlib::PlotSheet> sheets{};
    std::vector<std::string> warnings{};  // sheets that overflow their paper
};
struct BuildOptions {
    // The open drawing and its file: entries naming that file use it as it is now, unsaved
    // changes included, instead of reading the file.
    const Document* current = nullptr;
    std::filesystem::path currentFile{};
    // Where a page setup override is looked up when the sheet's drawing does not have it.
    const Document* setupSource = nullptr;
    // Reads a drawing; the default reads .tbcad files.
    std::function<Result<std::unique_ptr<Document>>(const std::filesystem::path&)> load{};
    // Called for each finished sheet (a plot stamp).
    std::function<void(exportlib::PlotSheet&, const SheetEntry&, const exportlib::PlotSettings&)>
        decorate{};
};

// Builds one sheet per entry (each drawing is loaded once). The error names the entry that failed.
Result<SheetSet> buildSheetList(const std::vector<SheetEntry>& entries,
                                const exportlib::PlotSettings& base,
                                const layout::StyleResolver& styles = {},
                                const BuildOptions& options = {});

// A layout (or model space) with a page setup is plotted with that setup instead of `base`;
// `styles` finds the setup's plot style table.
// Builds one sheet per entry (each drawing is loaded once) and writes them as the pages of one PDF.
// Nothing is written if any sheet fails; the error names the entry.
Result<SheetSetReport> plotSheetList(const std::vector<SheetEntry>& entries,
                                     const exportlib::PlotSettings& base,
                                     const std::filesystem::path& pdfFile,
                                     const layout::StyleResolver& styles = {},
                                     const BuildOptions& options = {});

}  // namespace tbcad::sheets
