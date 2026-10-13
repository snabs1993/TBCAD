// Plotting: turns the model-space entities of a Document into paper-space primitives (millimetres
// on the sheet) and writes them as PDF or SVG.
//
// Scale convention: `scaleN` N means 1:N, i.e. 1 mm on paper = N drawing units. Internally the
// layout keeps `paperPerUnit = 1 / N`. Lineweights are absolute on paper; linetype dashes are
// scaled by ltScale x entity scale (already in DisplayStyle::patternScale) and the plot scale.
#pragma once

#include <string>
#include <variant>
#include <vector>

#include "tbcad/core/core.h"

namespace tbcad::exportlib {

struct PaperSize {
    std::string name{};
    double shortMm = 0;  // portrait width
    double longMm = 0;   // portrait height
};

// ISO A0-A4 and ANSI A-E.
const std::vector<PaperSize>& paperSizes();
const PaperSize* findPaperSize(const std::string& name);
// The scale presets of the plot dialog (the N of 1:N).
const std::vector<double>& scalePresets();

enum class PlotArea { Extents, Display, Window };
enum class ColorMode { AsDisplayed, Monochrome, Greyscale };

struct PlotSettings {
    std::string paperName = "A3";
    double customWidthMm = 297, customHeightMm = 420;  // used when paperName is "Custom"
    bool landscape = true;
    PlotArea area = PlotArea::Extents;
    // Window / Display area in drawing units (the caller passes the current view for Display).
    BBox2 window{};
    bool fitToPaper = true;
    double scaleN = 1;  // 1:N, used when fitToPaper is false
    bool centerOnPaper = true;
    Vec2 offsetMm{};  // from the lower-left corner of the printable area, when not centred
    double marginMm = 10;
    bool lineweights = true;
    ColorMode color = ColorMode::AsDisplayed;
    // Used for lineweight 0 ("thinnest") and when lineweights are off.
    double hairlineMm = 0.1;
    double chordTolMm = 0.01;  // on paper
};

struct PlotLayout {
    double pageWidthMm = 0, pageHeightMm = 0;
    // Printable rectangle on the sheet (page minus margins), lower-left origin.
    BBox2 printable{};
    BBox2 area{};             // plotted rectangle in drawing units
    double paperPerUnit = 1;  // paper mm per drawing unit
    // Paper position of area.min.
    Vec2 origin{};
    // True when the scaled area is larger than the printable rectangle (it is clipped).
    bool overflows = false;

    Vec2 toPaper(Vec2 p) const {
        return {(p.x - area.min.x) * paperPerUnit + origin.x,
                (p.y - area.min.y) * paperPerUnit + origin.y};
    }
};

Result<PlotLayout> computeLayout(const Document& doc, const PlotSettings& settings);

// ---- paper-space primitives ---------------------------------------------------------------------

struct PlotStyle {
    Rgba color{};
    double widthMm = 0.25;
    std::vector<double> dashMm{};  // alternating on/off lengths in paper mm; empty = continuous
};

struct PlotPath {
    std::vector<Vec2> pts{};
    bool closed = false;
    PlotStyle style{};
};
struct PlotArc {    // counter-clockwise from start to end in drawing space; a full circle when full
    Vec2 center{};  // paper mm
    double radius = 0;
    double start = 0, end = 0;
    bool full = false;
    PlotStyle style{};
};
struct PlotFill {  // even-odd
    std::vector<std::vector<Vec2>> rings{};
    Rgba color{};
};
struct PlotDots {
    std::vector<Vec2> pts{};
    PlotStyle style{};
};
using PlotPrimitive = std::variant<PlotPath, PlotArc, PlotFill, PlotDots>;

struct PlotSheet {
    PlotLayout layout{};
    std::vector<PlotPrimitive> prims{};
    std::size_t entityCount = 0;  // entities plotted
};

// Plot-time colour: white / near-white becomes black (the sheet is always white), then the colour
// mode is applied.
Rgba plotColor(Rgba c, ColorMode mode);

// Builds the paper-space primitives, in draw order. Entities on layers with plot off, frozen /
// off layers and invisible entities are skipped. Display lists are generated at the plot
// resolution (chordTolMm on paper).
Result<PlotSheet> buildSheet(const Document& doc, const PlotSettings& settings);

Result<void> writePdf(const PlotSheet& sheet, const std::string& path,
                      const std::string& title = "TBCAD drawing", bool compress = true);
// For tests: the whole PDF in memory.
Result<std::string> pdfBytes(const PlotSheet& sheet, bool compress = false);
// SVG in millimetres: width/height in mm, viewBox in mm, y flipped (SVG y runs downwards).
std::string svgString(const PlotSheet& sheet);
Result<void> writeSvg(const PlotSheet& sheet, const std::string& path);

}  // namespace tbcad::exportlib
