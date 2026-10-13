// Internal: the DXF -> Document importer. Public entry point is importDxf() in reader.h.
#pragma once

#include <array>
#include <cstddef>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "records.h"
#include "tbcad/core/document.h"
#include "tbcad/dxf/lexer.h"
#include "tbcad/dxf/reader.h"

namespace tbcad::dxf::detail {

// ACI (AutoCAD colour index) to the nearest entry of the TBCAD palette. 1..7 map to themselves.
Color mapAci(int aci);

// Converts DXF MTEXT markup (already decoded to UTF-8) to TBCAD markup (docs/dxf-import.md).
// `dropped` counts the formatting codes that were removed, keyed by code letter.
std::string convertMText(std::string_view utf8, double unitScale, std::map<char, int>& dropped);

// Frame of an entity's object coordinate system: either the WCS or the WCS mirrored about the
// Y axis (extrusion (0, 0, -1)).
struct Frame {
    bool mirror = false;
    Vec2 p(Vec2 v) const { return mirror ? Vec2{-v.x, v.y} : v; }
    double angle(double a) const { return mirror ? kPi - a : a; }
    double bulge(double b) const { return mirror ? -b : b; }
};

struct LayerRow {
    std::string name{};
    int flags = 0;
    int color = 7;
    std::optional<Color> trueColor{};
    std::string linetype{};
    bool plot = true;
    int lineweight = -3;
    int line = 0;
    // TBCAD XDATA
    std::optional<std::string> xParent{};
    std::optional<std::string> xName{};
    int xFlags = 0;  // bit0 colour, bit1 linetype, bit2 lineweight are ByParent
};

// A level named by the TBCAD XDATA on the APPID record (see docs/dxf-import.md).
struct LevelRow {
    std::string name{};
    double elevation = 0;
    int flags = 1;  // bit0 visible, bit1 locked
};

// Removes the text records from a proxy graphics stream (see proxy.cpp).
void stripProxyText(std::vector<std::uint8_t>& raw);

class Importer {
public:
    Importer(std::span<const std::byte> bytes, const ImportOptions& options, Report& report)
        : bytes_(bytes), opts_(options), report_(report) {}

    Result<ImportResult> run();

private:
    // ---- import.cpp: file structure, tables, blocks -------------------------------------
    std::string dec(std::string_view raw) const;
    void parseHeader(const Record& rec);
    void setUnits(int insunits, bool assumed);
    void processTables(RecordReader& rr);
    void readLinetype(const Record& r);
    void readStyle(const Record& r);
    void readDimStyle(const Record& r);
    void readLayerRow(const Record& r);
    void readAppId(const Record& r);
    void createLevels();
    LevelId levelFor(const Record& r);
    void createLayers();
    void createLayerRows(const std::vector<LayerRow>& rows);
    LayerId ensureLayerPath(const std::vector<std::string>& segments, const LayerRow* leaf);
    void applyLayerRow(Layer& l, const LayerRow& row, bool hasParent);
    void processBlocks(RecordReader& rr);
    void processEntities(RecordReader& rr);
    void skipSection(RecordReader& rr);
    BlockId blockFor(std::string_view dxfName);
    void finish();

    // ---- entities.cpp: record -> entities ---------------------------------------------------
    void handleEntity(Record& r, RecordReader& rr, BlockId owner);
    bool convert(Record& r, RecordReader& rr, std::vector<Entity>& out, BlockId owner);
    void add(Entity&& e, BlockId owner, std::string_view type, int line);
    EntityCommon commonOf(const Record& r);
    bool frameOf(const Record& r, Frame& f, std::string_view type);
    LayerId layerFor(std::string_view raw, int line);
    LinetypeId linetypeFor(std::string_view raw, int line);
    TextStyleId textStyleFor(std::string_view raw) const;
    DimStyleId dimStyleFor(std::string_view raw) const;
    Vec2 P(const Record& r, int cx, int cy) const { return {r.num(cx) * k_, r.num(cy) * k_}; }
    void note3d(const Record& r);
    void bad(const Record& r, std::string_view type, const std::string& message);

    bool convLine(const Record& r, std::vector<Entity>& out);
    bool convRay(const Record& r, bool infinite, std::vector<Entity>& out);
    bool convCircle(const Record& r, const Frame& f, std::vector<Entity>& out);
    bool convArc(const Record& r, const Frame& f, std::vector<Entity>& out);
    bool convLwPolyline(const Record& r, const Frame& f, std::vector<Entity>& out);
    bool convPolyline(const Record& r, RecordReader& rr, const Frame& f, std::vector<Entity>& out);
    bool convEllipse(const Record& r, const Frame& f, std::vector<Entity>& out);
    bool convSpline(const Record& r, std::vector<Entity>& out);
    enum class TextKind { Text, Attrib, AttDef };
    bool convText(const Record& r, const Frame& f, TextKind kind, std::vector<Entity>& out);
    bool convMText(const Record& r, const Frame& f, std::vector<Entity>& out);
    bool convPoint(const Record& r, std::vector<Entity>& out);
    bool convInsert(const Record& r, RecordReader& rr, const Frame& f, std::vector<Entity>& out);
    bool blockHasAttDef(BlockId block, const std::string& tag) const;
    bool convSolid(const Record& r, const Frame& f, std::vector<Entity>& out);
    bool convLeader(const Record& r, std::vector<Entity>& out);
    bool convProxy(const Record& r, std::vector<Entity>& out);
    // An entity of another application (AEC, Civil, ACIS ...) that carries proxy graphics.
    bool convForeign(const Record& r, std::vector<Entity>& out);

    // ---- proxy.cpp --------------------------------------------------------------------------
    std::vector<std::uint8_t> proxyGraphicsOf(const Record& r) const;
    std::size_t decodeProxyGraphics(std::span<const std::uint8_t> data, const EntityCommon& base,
                                    ProxyData& proxy, std::vector<Entity>& texts,
                                    std::string& problem);
    DisplayStyle resolveProxyStyle(const EntityCommon& base, Color color);

    // ---- hatch.cpp --------------------------------------------------------------------------
    bool convHatch(const Record& r, const Frame& f, std::vector<Entity>& out);

    // ---- dimension.cpp ----------------------------------------------------------------------
    bool convDimension(const Record& r, std::vector<Entity>& out, BlockId owner);
    void appendDimensionGraphics(const Record& r, const Entity& dim, std::vector<Entity>& out);

    // ---- state ------------------------------------------------------------------------------
    std::span<const std::byte> bytes_;
    const ImportOptions& opts_;
    Report& report_;
    Encoding enc_ = Encoding::Cp1252;
    std::string acadVer_{};
    std::unique_ptr<Document> doc_{};
    std::optional<Transaction> tx_{};
    ImportSummary sum_{};
    double k_ = 1;  // file units -> mm

    // tables
    std::vector<LayerRow> layerRows_{};
    std::vector<std::string> layerRawNames_{};  // raw bytes of each row's name
    bool layersCreated_ = false;
    std::vector<LevelRow> levelRows_{};
    std::string levelCurrent_{};
    int levelView_ = 0;
    bool levelsCreated_ = false;
    std::unordered_map<std::string, LevelId> levelByFold_{};    // folded level name
    std::map<std::string, LayerId, std::less<>> layerByRaw_{};  // raw layer name bytes
    std::unordered_map<std::string, LayerId> layerByFold_{};    // folded decoded DXF name
    std::set<LayerId> placeholderLayers_{};
    std::map<std::string, LinetypeId, std::less<>> ltypeByRaw_{};
    std::unordered_set<std::string> warnedMissing_{};
    std::unordered_set<std::string> foreignShown_{};  // object types already reported as proxies

    // blocks
    BlockId curBlock_{};
    bool inBlock_ = false;
    bool skipBlock_ = false;
    std::unordered_map<std::string, BlockId> blockByFold_{};
    std::unordered_set<BlockId> blockDefined_{};

    // diagnostics accumulated for one summary line each
    std::map<std::string, std::pair<std::size_t, int>> skippedTypes_{};  // type -> (count, line)
    std::map<char, int> mtextDropped_{};
    std::size_t flattened_ = 0;
    std::size_t paperSpace_ = 0;
    int firstFlattenedLine_ = 0;
    std::size_t mirroredText_ = 0;
    int firstMirroredLine_ = 0;
};

}  // namespace tbcad::dxf::detail
