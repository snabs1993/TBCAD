// Plot frames: named areas of model space that remember the paper, orientation and scale of the
// sheet they plot to, so a drawing with many sheets side by side in model space (each at its own
// scale) can be plotted sheet by sheet or all at once. Frames live in
// Header::custom["tbcad.plotframes"], so they are saved with the drawing and changes are undoable.
#pragma once
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "tbcad/core/core.h"
#include "tbcad/export/plot.h"
#include "tbcad/layout/pagesetup.h"

namespace tbcad::layout {

inline constexpr const char* kPlotFrameKey = "tbcad.plotframes";

struct PlotFrame {
    std::string name{};
    BBox2 window{};                // model space area (drawing units)
    std::string paperName = "A3";  // a name from exportlib::paperSizes() or "Custom"
    double customWidthMm = 297, customHeightMm = 420;
    bool landscape = true;
    bool fitToPaper = false;
    double scaleN = 100;  // 1:N when not fit
    // A named page setup supplying the other plot options (style table, lineweights, colour
    // mode, margin). Empty: the current plot settings.
    std::string pageSetup{};
};

// All frames in creation order.
std::vector<PlotFrame> plotFrames(const Document& doc);
std::optional<PlotFrame> findPlotFrame(const Document& doc, std::string_view name);
// 1-64 characters, no control characters, commas or leading / trailing space; unique ignoring
// case.
Result<void> validatePlotFrameName(const Document& doc, std::string_view name,
                                   std::string_view ignore = {});
// Creates the frame, or replaces the one with the same name (ignoring case) in its place.
Result<void> savePlotFrame(Transaction& tx, const PlotFrame& frame);
Result<void> deletePlotFrame(Transaction& tx, const std::string& name);
Result<void> renamePlotFrame(Transaction& tx, const std::string& name, const std::string& newName);
// The first free "Sheet N".
std::string nextPlotFrameName(const Document& doc);
// The model space box a sheet covers at 1:scaleN: the paper less the margin on each side, times
// scaleN, with its lower-left corner at `corner`. Empty for an unknown paper.
BBox2 frameBoxForPaper(const std::string& paperName, bool landscape, double marginMm, double scaleN,
                       Vec2 corner, double customWidthMm = 297, double customHeightMm = 420);
// The settings to plot the frame with: `base` (or the frame's page setup when it names one that
// exists), then the frame's window, paper, orientation and scale, centred on the paper, with
// annotative objects at the frame's scale.
exportlib::PlotSettings frameSettings(const Document& doc, const exportlib::PlotSettings& base,
                                      const PlotFrame& frame, const StyleResolver& resolver = {});
// "A3 landscape 1:100" (or "Fit").
std::string describeFrame(const PlotFrame& frame);

}  // namespace tbcad::layout
