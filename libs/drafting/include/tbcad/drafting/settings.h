#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "tbcad/cmd/host.h"

namespace tbcad::drafting {

// Object snap modes in priority order: when two candidates are equally far from the cursor the
// one with the lower value wins (endpoint > intersection > midpoint > center > quadrant >
// perpendicular > tangent > nearest).
enum class SnapMode : std::uint8_t {
    Endpoint,
    Intersection,
    Midpoint,
    Center,
    Quadrant,
    Perpendicular,
    Tangent,
    Nearest,
    Node,                  // Point entities (always on while OSNAP is on)
    Insertion,             // insertion point of text, mtext, blocks
    ApparentIntersection,  // where two objects would meet if extended, near the cursor
    Extension,             // along the extension of a line or arc end the cursor rested on
};
inline constexpr int kSnapModeCount = 12;

using SnapModeMask = std::uint32_t;
constexpr SnapModeMask maskOf(SnapMode m) {
    return SnapModeMask{1} << static_cast<unsigned>(m);
}
inline constexpr SnapModeMask kAllSnapModes = (SnapModeMask{1} << kSnapModeCount) - 1;
// Endpoint, midpoint, center, intersection, quadrant and node.
inline constexpr SnapModeMask kDefaultSnapModes =
    maskOf(SnapMode::Endpoint) | maskOf(SnapMode::Midpoint) | maskOf(SnapMode::Center) |
    maskOf(SnapMode::Intersection) | maskOf(SnapMode::Quadrant) | maskOf(SnapMode::Node);

// "endpoint", "midpoint", ... (also the FilterFeedback::marker names).
std::string_view snapModeName(SnapMode m);
// "Endpoint", "Midpoint", ... (tooltip label).
std::string_view snapModeLabel(SnapMode m);
// Accepts the names above, case-insensitive, and unique prefixes ("end", "mid", "int", "per").
std::optional<SnapMode> parseSnapMode(std::string_view text);
// "endpoint,midpoint"; "none" for an empty mask.
std::string formatSnapModes(SnapModeMask mask);
// Comma / space separated names, plus "all" and "none". nullopt when a word is unknown.
std::optional<SnapModeMask> parseSnapModes(std::string_view text);

// Setting keys (stored through cmd::IHost::setSetting; the application persists them).
namespace keys {
inline constexpr const char* osnap = "drafting.osnap";                   // bool
inline constexpr const char* ortho = "drafting.ortho";                   // bool
inline constexpr const char* polar = "drafting.polar";                   // bool
inline constexpr const char* otrack = "drafting.otrack";                 // bool
inline constexpr const char* snap = "drafting.snap";                     // bool
inline constexpr const char* dynUcs = "drafting.dynucs";                 // bool
inline constexpr const char* grid = "view.grid";                         // bool
inline constexpr const char* dynInput = "drafting.dynInput";             // bool, on by default
inline constexpr const char* osnapModes = "drafting.osnap.modes";        // "endpoint,midpoint,..."
inline constexpr const char* osnapAperture = "drafting.osnap.aperture";  // pixels
inline constexpr const char* snapSpacing = "drafting.snap.spacing";      // mm
inline constexpr const char* gridSpacing = "drafting.grid.spacing";      // mm
inline constexpr const char* gridStyle = "drafting.grid.style";          // "dots" | "lines"
inline constexpr const char* osnapOnce = "drafting.osnap.once";  // "endpoint": next point only
inline constexpr const char* ucsIcon = "drafting.ucsicon";               // bool, on by default
inline constexpr const char* ucsIconOrigin = "drafting.ucsicon.origin";  // bool, on by default
inline constexpr const char* polarIncrement = "drafting.polar.increment";  // degrees, list
}  // namespace keys

// Everything the drafting aids read, taken from the host settings on every use so a change in
// the settings dialog or a command applies at once. Missing keys give the defaults below.
struct DraftingSettings {
    bool osnap = true;
    bool ortho = false;
    bool polar = false;
    bool otrack = false;  // object snap tracking
    bool snap = false;
    bool dynUcs = false;  // dynamic UCS: ortho, polar and snap follow the edge under the cursor
    bool grid = false;
    bool dynInput = true;
    SnapModeMask modes = kDefaultSnapModes;
    double apertureMm = 0;  // unused by read(); filled from aperturePx and the pixel size
    double aperturePx = 10;
    double snapSpacing = 10;
    double gridSpacing = 10;
    bool gridLines = false;  // false = dots
    // Each entry is an increment: polar tracking snaps to every multiple of every entry.
    std::vector<double> polarIncrementsDeg{90};
    double polarToleranceDeg = 2;

    static DraftingSettings read(const cmd::IHost& host);
};

// "90", "45", "30,45" -> {90} ... Empty on error. Values must be in (0, 180].
std::vector<double> parsePolarIncrements(std::string_view text);
bool parseBoolSetting(std::string_view text, bool fallback);

}  // namespace tbcad::drafting
