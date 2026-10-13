// DXF export: turns a TBCAD Document into the bytes of a DXF file (see docs/dxf-export.md).
#pragma once

#include <cstddef>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "tbcad/core/document.h"
#include "tbcad/dxf/report.h"

namespace tbcad::dxf {

enum class DxfVersion {
    R12,    // AC1009, ASCII, no handles; the oldest format other programs still read
    R2000,  // AC1015, ASCII with \U+nnnn escapes (the default, accepted everywhere)
    R2007,  // AC1021, UTF-8 text
};

// "AC1009" / "AC1015" / "AC1021".
std::string_view versionCode(DxfVersion v);
// Parses "r12", "r2000", "r2007" (also "ac1015", "2000", ...); empty when unknown.
std::optional<DxfVersion> parseVersion(std::string_view text);

// One line of a hatch pattern definition in the .pat convention at scale 1 and angle 0: the angle
// of the line family, its origin, the shift along the line (dx) and the spacing across it (dy),
// then the dash lengths (+ dash, - gap, 0 dot; empty = continuous). All lengths in millimetres.
struct HatchPatternLine {
    double angleDeg = 0;
    double originX = 0, originY = 0;
    double dx = 0, dy = 0;
    std::vector<double> dashes{};
};

// Supplies the definition lines of a pattern by name (compared ASCII case-insensitively), or
// nothing when it does not know the name. W5-4 plugs its pattern library in here; without one
// the writer uses a small built-in set (LINE0, LINE45, CROSS45, GRID, BRICK, DOTS, SAND, EARTH).
using HatchPatternLookup =
    std::function<std::optional<std::vector<HatchPatternLine>>(std::string_view name)>;

struct ExportOptions {
    DxfVersion version = DxfVersion::R2000;
    // DXF layers are flat: a sub-layer is written as "<parent><separator><child>". Do not use
    // "|" (xref layers) or "$" (bound xrefs).
    std::string layerSeparator = "~";
    HatchPatternLookup patterns{};
};

struct ExportSummary {
    std::string acadVer{};  // "AC1015"
    std::string version{};  // "R2000"
    bool utf8 = false;
    std::map<std::string, std::size_t> entities{};  // DXF type name -> records written
    std::size_t layers = 0;
    std::size_t blocks = 0;
    std::size_t skipped = 0;     // entities that could not be written (see the report)
    std::size_t downgraded = 0;  // entities written in a simpler form (see the report)

    std::string toJson(const Report& report) const;
};

struct ExportResult {
    std::string bytes{};
    ExportSummary summary{};
};

// Fails only on invalid options (unknown separator characters). Everything else produces a file
// plus diagnostics in `report`.
Result<ExportResult> exportDxf(const Document& doc, const ExportOptions& options, Report& report);

// Stable diagnostic codes of the writer.
namespace wcode {
inline constexpr std::string_view kSkippedEntity = "dxf.writer.skipped_entity";
inline constexpr std::string_view kDowngraded = "dxf.writer.downgraded";
inline constexpr std::string_view kNameChanged = "dxf.writer.name_changed";
inline constexpr std::string_view kNoDimensionGraphics = "dxf.writer.dimension_graphics_missing";
inline constexpr std::string_view kUnknownPattern = "dxf.writer.unknown_pattern";
inline constexpr std::string_view kBadNumber = "dxf.writer.bad_number";
inline constexpr std::string_view kVersionLimit = "dxf.writer.version_limit";
inline constexpr std::string_view kLayerFlattened = "dxf.writer.layer_state_flattened";
}  // namespace wcode

}  // namespace tbcad::dxf
