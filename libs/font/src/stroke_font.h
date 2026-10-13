#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "tbcad/font/font.h"

namespace tbcad::font {

// A single-stroke font in TBCAD's own text format (.tbsf, see docs/font.md). Glyph geometry is
// stored in em units (design units / upm), y up.
class StrokeFont {
public:
    enum class Mark : std::uint8_t { None, Upper, Tone, Lower };

    struct Glyph {
        StrokeGlyph strokes;
        double advance = 0;  // em
        Mark mark = Mark::None;
    };

    // Returns an empty message on success.
    std::string parse(std::string_view text);

    bool has(char32_t cp) const { return glyphs_.count(cp) != 0; }
    const Glyph* glyph(char32_t cp) const {
        auto it = glyphs_.find(cp);
        return it == glyphs_.end() ? nullptr : &it->second;
    }
    const FontMetrics& metrics() const { return metrics_; }
    const std::string& family() const { return family_; }
    int unitsPerEm() const { return upm_; }
    double toneDrop() const { return toneDrop_; }

private:
    std::map<char32_t, Glyph> glyphs_;
    FontMetrics metrics_;
    std::string family_;
    int upm_ = 20;
    double toneDrop_ = 0;  // em
};

}  // namespace tbcad::font
