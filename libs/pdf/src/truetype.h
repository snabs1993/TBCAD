// TrueType font subsetting for embedding in PDF (internal to the pdf library).
#pragma once

#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace tbcad::pdf::detail {

// What the PDF font dictionaries need from a TrueType font, in font units.
struct TrueTypeInfo {
    int unitsPerEm = 1000;
    int numGlyphs = 0;
    int xMin = 0, yMin = 0, xMax = 0, yMax = 0;  // head bounding box
    int ascent = 0, descent = 0;                 // hhea
    int capHeight = 0;                           // OS/2 (version 2+), else the ascent
    double italicAngle = 0;                      // post
    bool fixedPitch = false;
    std::vector<std::uint16_t> advances{};       // hmtx, one per glyph
    std::string postscriptName{};                // name ID 6; empty when the font has none
};

// Reads the face `faceIndex` of a .ttf / .ttc file. Empty when it is not a TrueType font with
// glyph outlines ('glyf'; CFF-flavoured OpenType is not embedded this way) or is damaged.
std::optional<TrueTypeInfo> readTrueType(const std::vector<unsigned char>& file, int faceIndex);

// A standalone .ttf holding only the tables a PDF viewer needs (head, hhea, maxp, hmtx, loca,
// glyf, cvt, fpgm, prep) in which every glyph not in `keep` (nor used by a kept composite glyph)
// is empty. Glyph ids are unchanged, so the PDF can address glyphs by their ids. Empty on error.
std::string subsetTrueType(const std::vector<unsigned char>& file, int faceIndex,
                           const std::set<std::uint16_t>& keep);

}  // namespace tbcad::pdf::detail
