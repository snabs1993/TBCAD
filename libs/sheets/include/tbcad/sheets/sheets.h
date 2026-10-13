#pragma once

// Sheet lists: a text file naming the layouts to plot, in order, possibly from several drawings.
//
//   # comment
//   plans/ground.tbcad | A1 Plan
//   plans/roof.tbcad                  <- no layout: model space with the plot settings
//
// Relative paths are relative to the list file.

#include <filesystem>
#include <string>
#include <vector>

#include "tbcad/core/result.h"
#include "tbcad/export/plot.h"
#include "tbcad/layout/pagesetup.h"

namespace tbcad::sheets {

struct SheetEntry {
    std::filesystem::path file{};
    std::string layout{};  // empty: model space
};

// Parses the list text; `baseDir` resolves relative file names. A bad line is an error naming it.
Result<std::vector<SheetEntry>> parseSheetList(const std::string& text,
                                               const std::filesystem::path& baseDir);
// Reads and parses a list file.
Result<std::vector<SheetEntry>> readSheetList(const std::filesystem::path& listFile);

struct SheetSetReport {
    std::size_t pages = 0;
    std::vector<std::string> warnings{};  // sheets that overflow their paper
};

struct SheetSet {
    std::vector<exportlib::PlotSheet> sheets{};
    std::vector<std::string> warnings{};  // sheets that overflow their paper
};
// Builds one sheet per entry (each drawing is loaded once). The error names the entry that failed.
Result<SheetSet> buildSheetList(const std::vector<SheetEntry>& entries,
                                const exportlib::PlotSettings& base,
                                const layout::StyleResolver& styles = {});

// A layout (or model space) with a page setup is plotted with that setup instead of `base`;
// `styles` finds the setup's plot style table.
// Builds one sheet per entry (each drawing is loaded once) and writes them as the pages of one PDF.
// Nothing is written if any sheet fails; the error names the entry.
Result<SheetSetReport> plotSheetList(const std::vector<SheetEntry>& entries,
                                     const exportlib::PlotSettings& base,
                                     const std::filesystem::path& pdfFile,
                                     const layout::StyleResolver& styles = {});

}  // namespace tbcad::sheets
