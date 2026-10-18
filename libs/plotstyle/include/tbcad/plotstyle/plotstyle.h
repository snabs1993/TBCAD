#pragma once

// Plot style tables. A table has one entry for each of the 255 palette colours (1..255); at plot
// time an object's resolved colour selects an entry, and the entry can override the plotted
// colour, lineweight and screening. True colours take the colour and screening of the nearest
// palette colour (their lineweight stays their own); "Use object color" entries keep them as they
// are.
//
// Text format (`.tbstyle`, UTF-8, one line per fact):
//
//   TBCAD-PLOT-STYLE 1
//   name monochrome
//   description All objects black
//   1 color=#000000 lineweight=0.25 screen=100 gray=0
//   2 color=object lineweight=object screen=50
//
// Entries that are not listed use the object's colour and lineweight at 100% screening.

#include <array>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "tbcad/core/color.h"
#include "tbcad/core/result.h"

namespace tbcad::plotstyle {

struct PlotStyleEntry {
    std::optional<Rgba> color{};           // nullopt: use the object's colour
    std::optional<double> lineweightMm{};  // nullopt: use the object's lineweight
    int screen = 100;                      // 0..100 %: 100 is full strength, less is lighter
    bool grayscale = false;                // convert the colour to a grey of the same lightness
    std::string name{};                    // optional label (CTB "Color_1" ...)

    friend bool operator==(const PlotStyleEntry&, const PlotStyleEntry&) = default;
};

struct PlotStyleTable {
    std::string name{};
    std::string description{};
    std::array<PlotStyleEntry, 256> entries{};  // index 0 is unused

    const PlotStyleEntry& forIndex(int aci) const {
        return entries[static_cast<std::size_t>(aci & 255)];
    }
    friend bool operator==(const PlotStyleTable&, const PlotStyleTable&) = default;
};

// Palette index (1..255) of a resolved colour, or 0 when it is not a palette colour.
int paletteIndexOf(Rgba color);

// Palette index (1..255) closest to a colour, so true colours follow the table entry that
// looks most like them (AutoCAD does the same). The exact index when the colour is a palette
// colour.
int nearestPaletteIndex(Rgba color);
// The table entry a true colour plots with: the nearest of the colours 1 to 249 (the grey ramp
// 250 to 255 is left out, so a dark grey true colour takes the black pen of colour 8 as it does in
// AutoCAD's plots of the office table). Palette colours give their own index.
int penIndexOfTrueColor(Rgba color);

// The entry's colour override (or `base`), made grey and screened as the entry says.
Rgba applyEntry(const PlotStyleEntry& entry, Rgba base);
// The colour that is plotted for an object drawn in `objectColor`: the table entry's colour (or the
// object's), screened towards white and optionally grey. Without a palette match the colour is
// returned unchanged.
Rgba mapColor(const PlotStyleTable& table, Rgba objectColor);
// The lineweight in mm for an object drawn in `objectColor` whose own weight is `objectMm`.
double mapLineweight(const PlotStyleTable& table, Rgba objectColor, double objectMm);

// Built-in tables: "monochrome.ctb" (everything black), "grayscale.ctb", "screening-25.ctb" ...
// Names compare ignoring case. nullopt for unknown names.
std::optional<PlotStyleTable> builtinTable(const std::string& name);
std::vector<std::string> builtinTableNames();
inline constexpr const char* kDefaultTable = "monochrome.ctb";

std::string toText(const PlotStyleTable& table);
Result<PlotStyleTable> fromText(const std::string& text);

// Reads a Plot style table from AutoCAD-style `.ctb` bytes (compressed text format). Only the
// colour-dependent tables are supported; `.stb` (named styles) are refused with a clear error.
Result<PlotStyleTable> importCtb(std::span<const std::uint8_t> bytes);
// The decompressed text of a `.ctb` file, for inspection (what a pen really stores).
Result<std::string> ctbText(std::span<const std::uint8_t> bytes);

// Loads `.tbstyle` text or a `.ctb` file by its extension.
Result<PlotStyleTable> loadFile(const std::filesystem::path& path);
Result<void> saveFile(const PlotStyleTable& table, const std::filesystem::path& path);

}  // namespace tbcad::plotstyle
