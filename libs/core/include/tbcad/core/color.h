#pragma once

#include <cstdint>

namespace tbcad {

struct Rgba {
    std::uint8_t r = 0, g = 0, b = 0, a = 255;
    friend constexpr bool operator==(const Rgba&, const Rgba&) = default;
};

enum class ColorKind : std::uint8_t { ByLayer, ByBlock, Index, Rgb };

// ByLayer | ByBlock | palette index (1..255) | RGB. For Index the index is kept in `r`.
struct Color {
    ColorKind kind = ColorKind::ByLayer;
    std::uint8_t r = 0, g = 0, b = 0;

    static constexpr Color byLayer() { return {}; }
    static constexpr Color byBlock() { return {ColorKind::ByBlock, 0, 0, 0}; }
    static constexpr Color index(std::uint8_t i) { return {ColorKind::Index, i, 0, 0}; }
    static constexpr Color rgb(std::uint8_t r, std::uint8_t g, std::uint8_t b) {
        return {ColorKind::Rgb, r, g, b};
    }

    constexpr std::uint8_t indexValue() const { return r; }
    friend constexpr bool operator==(const Color&, const Color&) = default;
};

// ByLayer | ByBlock | Default | value in hundredths of a millimetre.
struct LineWeight {
    std::int16_t v = -1;

    static constexpr LineWeight byLayer() { return {-1}; }
    static constexpr LineWeight byBlock() { return {-2}; }
    static constexpr LineWeight defaultWeight() { return {-3}; }
    static constexpr LineWeight hundredths(std::int16_t h) { return {h}; }
    static LineWeight mm(double millimetres);

    constexpr bool isValue() const { return v >= 0; }
    constexpr bool isByLayer() const { return v == -1; }
    constexpr bool isByBlock() const { return v == -2; }
    constexpr bool isDefault() const { return v == -3; }
    // Weight in mm; 0 for the symbolic values.
    constexpr double toMm() const { return v >= 0 ? v / 100.0 : 0.0; }
    friend constexpr bool operator==(const LineWeight&, const LineWeight&) = default;
};

// TBCAD's own 256-entry palette. Index 0 is unused (returns black).
// 1..7: red, yellow, green, cyan, blue, magenta, white (white is drawn dark on light backgrounds
// by the renderer). 8..9: grays. 10..249: 24 hues in 15-degree steps, 10 shades each
// (see color.cpp). 250..255: grays.
Rgba paletteColor(std::uint8_t index);

}  // namespace tbcad
