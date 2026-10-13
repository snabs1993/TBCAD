// DXF import: turns the bytes of a DXF file into a TBCAD Document (see docs/dxf-import.md).
#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>

#include "tbcad/core/document.h"
#include "tbcad/dxf/encoding.h"
#include "tbcad/dxf/report.h"

namespace tbcad::dxf {

struct ImportOptions {
    // Force the text encoding instead of detecting it.
    std::optional<Encoding> encoding{};
    // Force the meaning of the drawing units (a $INSUNITS code, e.g. 4 = mm, 1 = inch).
    std::optional<int> insunits{};
    // When not empty, layer names such as "S-FLOOR~edge" are split on this separator into a
    // parent layer and a sub-layer. Sub-layers stored in TBCAD XDATA are restored regardless.
    std::string splitLayers{};
    // Keep the graphics of the anonymous *D block of every dimension next to the
    // DimensionData entity (ProxyData graphics + loose text). Off by default: dimensions draw
    // themselves from their definition points (W5-5). Turn it on to see exactly what the
    // other program drew.
    bool dimensionGraphics = false;
};

struct EntityCount {
    std::size_t read = 0;      // records of this type found in model space and blocks
    std::size_t imported = 0;  // TBCAD entities produced from them
    std::size_t skipped = 0;   // records dropped (unsupported, degenerate, rejected)
};

struct ImportSummary {
    std::string acadVer{};  // "AC1015" or empty
    std::string version{};  // "R2000", "R12", ... or empty
    Encoding encoding = Encoding::Cp1252;
    bool encodingGuessed = false;
    bool binary = false;
    int insunits = 0;           // as found in the file (0 = absent / unitless)
    std::string unitsName{};    // "millimeters", ...
    double unitScale = 1;       // multiplier applied to every length (file unit -> mm)
    bool unitsAssumed = false;  // true when the file did not say and mm was assumed
    std::map<std::string, EntityCount> entities{};  // by DXF type name
    std::size_t layers = 0;
    std::size_t blocks = 0;
    std::size_t outsideWorkArea = 0;  // entities kept but beyond the working area
    std::size_t beyondHardCap = 0;    // entities dropped because of the hard coordinate cap

    // Full JSON report: {"input":..,"units":..,"entities":..,"summary":..,"report":{...}}.
    std::string toJson(const Report& report) const;
};

struct ImportResult {
    std::unique_ptr<Document> document{};
    ImportSummary summary{};
};

// Fails only when the bytes are not a DXF file at all or the version is older than R12
// (AC1009). Everything else produces a document plus diagnostics in `report`.
Result<ImportResult> importDxf(std::span<const std::byte> bytes, const ImportOptions& options,
                               Report& report);

// Stable diagnostic codes of the reader (in addition to those of the lexer and encoding).
namespace code {
inline constexpr std::string_view kUnsupportedVersion = "dxf.reader.unsupported_version";
inline constexpr std::string_view kUnitsAssumed = "dxf.reader.units_assumed";
inline constexpr std::string_view kUnitsScaled = "dxf.reader.units_scaled";
inline constexpr std::string_view kSkippedEntity = "dxf.reader.skipped_entity";
inline constexpr std::string_view kBadEntity = "dxf.reader.bad_entity";
inline constexpr std::string_view kExtrusion3d = "dxf.reader.extrusion_3d";
inline constexpr std::string_view kMirroredText = "dxf.reader.mirrored_text";
inline constexpr std::string_view kFlattened = "dxf.reader.flattened_3d";
inline constexpr std::string_view kMissingLayer = "dxf.reader.missing_layer";
inline constexpr std::string_view kMissingLinetype = "dxf.reader.missing_linetype";
inline constexpr std::string_view kMissingBlock = "dxf.reader.missing_block";
inline constexpr std::string_view kDuplicateBlock = "dxf.reader.duplicate_block";
inline constexpr std::string_view kLayerRejected = "dxf.reader.layer_rejected";
inline constexpr std::string_view kSubLayerParent = "dxf.reader.sublayer_parent_missing";
inline constexpr std::string_view kBeyondHardCap = "dxf.reader.beyond_hard_cap";
inline constexpr std::string_view kOutsideWorkArea = "dxf.reader.outside_work_area";
inline constexpr std::string_view kRejected = "dxf.reader.entity_rejected";
inline constexpr std::string_view kMTextMarkup = "dxf.reader.mtext_markup_dropped";
inline constexpr std::string_view kProxy = "dxf.reader.proxy_entity";
inline constexpr std::string_view kPaperSpace = "dxf.reader.paper_space";
inline constexpr std::string_view kGradient = "dxf.reader.gradient_hatch";
inline constexpr std::string_view kComplexLinetype = "dxf.reader.complex_linetype";
inline constexpr std::string_view kShxFont = "dxf.reader.shx_font_mapped";
inline constexpr std::string_view kNoDimensionBlock = "dxf.reader.dimension_block_missing";
inline constexpr std::string_view kXref = "dxf.reader.xref_block";
inline constexpr std::string_view kMissingSection = "dxf.reader.missing_section";
inline constexpr std::string_view kInsertArray = "dxf.reader.insert_array_too_large";
}  // namespace code

}  // namespace tbcad::dxf
