// DXF import: turns the bytes of a DXF file into a TBCAD Document (see docs/dxf-import.md).
#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "tbcad/core/document.h"
#include "tbcad/dxf/encoding.h"
#include "tbcad/dxf/lexer.h"
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
    // Diagnostics: compare what TBCAD draws for each dimension with the *D block the other
    // program stored, and list the differences in ImportSummary::dimCompare.
    bool dimensionCompare = false;
    // DWG only: add the long structure trace to the report (what was found where). The report
    // is for people diagnosing a file; opening a drawing leaves it off, as it takes seconds on a
    // big file.
    bool diagnostics = true;
    // DXF only: threads for reading (0 = as many as fit, 1 = none). A second thread splits the
    // text into records while the first builds the drawing; the result is the same.
    std::size_t maxThreads = 0;
};

struct EntityCount {
    std::size_t read = 0;      // records of this type found in model space and blocks
    std::size_t imported = 0;  // TBCAD entities produced from them
    std::size_t skipped = 0;   // records dropped (unsupported, degenerate, rejected)
};

// A named page setup of the file (a PLOTSETTINGS object), as stored; lengths in millimetres.
struct PlotSetupRecord {
    std::string name{};
    std::string paperSize{};   // the paper's name in the file
    std::string styleSheet{};  // the plot style table (.ctb / .stb)
    double paperWidthMm = 0, paperHeightMm = 0;
    int rotation = 0;     // 0, 1 (90 degrees), 2, 3 (270 degrees)
    int plotType = 1;     // 0 last screen, 1 extents, 2 limits, 3 view, 4 window, 5 layout
    int scaleType = 0;    // 0 fit to paper, otherwise a standard scale
    double scaleNum = 1;  // custom scale: paper units ...
    double scaleDen = 1;  // ... per drawing units
    bool centered = true;
    bool lineweights = true;
    double originX = 0, originY = 0;  // plot origin offset
    double windowMinX = 0, windowMinY = 0, windowMaxX = 0, windowMaxY = 0;
};

// How one dimension TBCAD draws differs from the block the other program stored for it.
// Distances are in multiples of the stored text height (or of the arrow size without text).
struct DimComparison {
    std::uint64_t handle = 0;
    int line = 0;
    std::string kind{};           // "linear", "aligned", "radius", ...
    std::string storedText{};     // without formatting codes
    std::string generatedText{};
    bool textMatch = true;
    double textOffset = 0;        // between the text centres
    double heightRatio = 1;       // generated / stored text height (1 when either has no text)
    double strayStored = 0;       // share of the stored lines' length TBCAD does not draw
    double strayGenerated = 0;    // share of TBCAD's lines the stored block does not have
    int arrowsStored = 0;         // arrowheads (solids, arrow blocks) in the stored block
    int arrowsUnmatched = 0;      // of those, how many have no arrowhead of TBCAD's nearby
    int arrowsGenerated = 0;
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
    std::size_t layouts = 0;          // paper space layouts imported
    std::size_t outsideWorkArea = 0;  // entities kept but beyond the working area
    std::size_t beyondHardCap = 0;    // entities dropped because of the hard coordinate cap
    std::vector<PlotSetupRecord> plotSetups{};    // named page setups
    std::vector<PlotSetupRecord> layoutSetups{};  // the plot settings each layout carries
    std::vector<DimComparison> dimCompare{};      // with ImportOptions::dimensionCompare

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

// The same for groups already in memory (what the DWG reader produces): no text is written
// and lexed again. Imports exactly what importDxf would from `groups.str()`.
Result<ImportResult> importDxfGroups(const GroupBuffer& groups, const ImportOptions& options,
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
inline constexpr std::string_view kTableCells = "dxf.reader.table_cells";
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
inline constexpr std::string_view kProxyGraphics = "dxf.reader.proxy_graphics";
inline constexpr std::string_view kProxyDamaged = "dxf.reader.proxy_graphics_damaged";
inline constexpr std::string_view kProxyUnknown = "dxf.reader.proxy_graphics_unknown_records";
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
