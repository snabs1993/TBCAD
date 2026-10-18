// Plotting: turns the model-space entities of a Document into paper-space primitives (millimetres
// on the sheet) and writes them as PDF or SVG.
//
// Scale convention: `scaleN` N means 1:N, i.e. 1 mm on paper = N drawing units. Internally the
// layout keeps `paperPerUnit = 1 / N`. Lineweights are absolute on paper; linetype dashes are
// scaled by ltScale x entity scale (already in DisplayStyle::patternScale) and the plot scale.
#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <variant>
#include <vector>

#include "tbcad/core/core.h"
#include "tbcad/plotstyle/plotstyle.h"

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
    // Every lineweight is multiplied by this (a heavier or lighter print of the same drawing).
    // Hairlines (weight 0, or lineweights off) are not scaled.
    double lineweightScale = 1;
    // Objects and layers with a transparency print translucent (blended with the white sheet).
    bool transparency = true;
    // Fills PlotSheet::trace with how each drawn colour was mapped to a pen (for diagnosing).
    bool traceColors = false;
    ColorMode color = ColorMode::AsDisplayed;
    // Plot style table: per palette colour, overrides colour, lineweight and screening. Applied
    // before `color` (Monochrome / Greyscale still win over it). Null plots as drawn.
    std::shared_ptr<const plotstyle::PlotStyleTable> styleTable{};
    // Used for lineweight 0 ("thinnest") and when lineweights are off.
    double hairlineMm = 0.1;
    double chordTolMm = 0.01;  // on paper
    // The drawing's file: relative paths of pictures (IMAGEATTACH) are taken from its folder.
    // Pictures whose file cannot be found are left out.
    std::filesystem::path hostFile{};
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
    // Scale N of 1:N for annotative objects; 0 = the drawing's current annotation scale. A layout
    // viewport sets its own, so the same text prints at one paper height in every viewport.
    double annoScale = 0;
    // The sheet's own objects (a layout), not model space: see geom::DisplayContext::paperSpace.
    bool paperSpace = false;
    // Layers hidden in this view only (frozen in a layout viewport). Checked for every drawn part,
    // so a block's contents on such a layer disappear too.
    std::function<bool(LayerId)> layerHidden{};

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
    std::uint32_t layer = 0;       // 1-based index into PlotSheet::layerNames; 0 = none
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
    std::uint32_t layer = 0;  // as in PlotStyle
};
struct PlotDots {
    std::vector<Vec2> pts{};
    PlotStyle style{};
};
// Clips everything between a begin and its end to a rectangle on the sheet (layout viewports).
struct PlotClip {
    BBox2 rect{};  // always the bounding box of the clip
    bool begin = true;
    std::vector<Vec2> poly{};  // three or more points: a non-rectangular clip (a clipped viewport)
};
// A picture (already cut to its clip rectangle): `corners` are the bottom-left, bottom-right,
// top-right and top-left of the pixels on the sheet, in paper mm. `opacity` 0..1.
struct PlotImage {
    std::shared_ptr<const ImageData> data{};
    std::array<Vec2, 4> corners{};
    double opacity = 1;
};
using PlotPrimitive = std::variant<PlotPath, PlotArc, PlotFill, PlotDots, PlotClip, PlotImage>;

struct PlotSheet {
    PlotLayout layout{};
    std::vector<PlotPrimitive> prims{};
    std::size_t entityCount = 0;  // entities plotted
    // With PlotSettings::traceColors: one line per drawn colour (palette entry, pen, result).
    std::vector<std::string> trace{};
    // Layers of the plotted objects, in order of first use; PlotStyle::layer counts from 1. Names
    // of bound external references read "xref|layer", as in AutoCAD's PDF.
    std::vector<std::string> layerNames{};
};

// Plot-time colour: white / near-white becomes black (the sheet is always white), then the colour
// mode is applied.
Rgba plotColor(Rgba c, ColorMode mode);

// Builds the paper-space primitives, in draw order. Entities on layers with plot off, frozen /
// off layers and invisible entities are skipped. Display lists are generated at the plot
// resolution (chordTolMm on paper).
Result<PlotSheet> buildSheet(const Document& doc, const PlotSettings& settings);

// Entities for which the filter returns true are left out.
using EntityFilter = std::function<bool(const Entity&)>;
// Plots entities (in the order given) through `layout` into `sheet`; the building block of
// buildSheet and of layout plotting, where each viewport has a layout of its own.
void appendEntities(PlotSheet& sheet, const Document& doc, const std::vector<EntityId>& ids,
                    const PlotLayout& layout, const PlotSettings& settings,
                    const EntityFilter& skip = {});

Result<void> writePdf(const PlotSheet& sheet, const std::string& path,
                      const std::string& title = "TBCAD drawing", bool compress = true);
// One page per sheet, each with its own paper size.
Result<void> writePdfPages(const std::vector<PlotSheet>& sheets, const std::string& path,
                           const std::string& title = "TBCAD drawing", bool compress = true);
// For tests: the whole PDF in memory.
Result<std::string> pdfBytes(const PlotSheet& sheet, bool compress = false);
// SVG in millimetres: width/height in mm, viewBox in mm, y flipped (SVG y runs downwards).
std::string svgString(const PlotSheet& sheet);
Result<void> writeSvg(const PlotSheet& sheet, const std::string& path);

// A rendered sheet: 8-bit RGB, rows top to bottom, no padding.
struct RasterImage {
    int width = 0;
    int height = 0;
    double dpi = 0;
    std::vector<std::uint8_t> rgb{};
};
// Renders the sheet, anti-aliased, on white paper at `dpi`. Fails when the image would exceed
// `maxPixels`.
Result<RasterImage> rasterize(const PlotSheet& sheet, double dpi,
                              std::int64_t maxPixels = 200'000'000);
// PNG (with a pHYs chunk carrying the dpi) and baseline JPEG (quality 1..100, JFIF density set).
std::string pngBytes(const RasterImage& img);
std::string jpegBytes(const RasterImage& img, int quality = 90);
Result<void> writePng(const PlotSheet& sheet, const std::string& path, double dpi);
Result<void> writeJpeg(const PlotSheet& sheet, const std::string& path, double dpi,
                       int quality = 90);

}  // namespace tbcad::exportlib
