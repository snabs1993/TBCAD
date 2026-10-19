// Internal: the DXF -> Document importer. Public entry point is importDxf() in reader.h.
#pragma once

#include <array>
#include <cmath>
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
#include "tbcad/mline/mline.h"

namespace tbcad::dxf::detail {

// ACI (AutoCAD colour index) to the nearest entry of the TBCAD palette. 1..7 map to themselves.
Color mapAci(int aci);

// Converts DXF MTEXT markup (already decoded to UTF-8) to TBCAD markup (docs/dxf-import.md).
// `dropped` counts the formatting codes that were removed, keyed by code letter.
std::string convertMText(std::string_view utf8, double unitScale, std::map<char, int>& dropped);

// Frame of an entity's object coordinate system seen from above: the WCS, the WCS mirrored about
// the Y axis (extrusion (0, 0, -1)), and either of these turned by `rot` (a slightly tilted
// extrusion: the arbitrary axis algorithm turns the object x axis, and the tilt itself is
// dropped by the projection onto the XY plane).
struct Frame {
    bool mirror = false;
    double rot = 0;
    Vec2 v(Vec2 a) const {  // a direction or offset
        if (mirror)
            a = Vec2{-a.x, a.y};
        if (rot == 0)
            return a;
        const double c = std::cos(rot), s = std::sin(rot);
        return Vec2{a.x * c - a.y * s, a.x * s + a.y * c};
    }
    Vec2 p(Vec2 a) const { return v(a); }
    double angle(double a) const { return rot + (mirror ? kPi - a : a); }
    double bulge(double b) const { return mirror ? -b : b; }
};

int transparencyFromGroup(std::int64_t v);

struct LayerRow {
    std::string name{};
    int flags = 0;
    int color = 7;
    std::optional<Color> trueColor{};
    std::string linetype{};
    bool plot = true;
    int lineweight = -3;
    int transparency = 0;       // percent
    std::string description{};  // AcAecLayerStandard XDATA
    int line = 0;
    std::string plotStyleHandle{};  // group 390: a plot style name object (named plot styles)
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
    // Groups already in memory instead of DXF bytes.
    Importer(const GroupBuffer& groups, const ImportOptions& options, Report& report)
        : bytes_(), groups_(&groups), opts_(options), report_(report) {}

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
    void readUcs(const Record& r);
    void createLevels();
    LevelId levelFor(const Record& r);
    void createLayers();
    void createLayerRows(const std::vector<LayerRow>& rows);
    LayerId ensureLayerPath(const std::vector<std::string>& segments, const LayerRow* leaf);
    void applyLayerRow(Layer& l, const LayerRow& row, bool hasParent);
    void processBlocks(RecordReader& rr);
    void processEntities(RecordReader& rr);
    void skipSection(RecordReader& rr);
    void processObjects(RecordReader& rr);
    void processClasses(RecordReader& rr);
    void storeObject(const Record& r);
    void importGroups();
    void keepObjects();
    void keepHandles();
    void applyPlotStyles();
    void readPlotSettings(const Record& r);
    PlotSetupRecord plotSetupOf(const Record& r, const std::string& name);
    BlockId blockFor(std::string_view dxfName);
    void finish();
    void finishLayouts();
    void readLayoutObject(const Record& r);
    void readViewport(const Record& r, BlockId sheetBlock);
    // The sheet (paper space block) with this folded DXF name; created on first use.
    std::size_t sheetFor(const std::string& foldedName);

    // ---- entities.cpp: record -> entities ---------------------------------------------------
    void handleEntity(Record& r, RecordReader& rr, BlockId owner);
    bool convert(Record& r, RecordReader& rr, std::vector<Entity>& out, BlockId owner);
    void add(Entity&& e, BlockId owner, std::string_view type, int line,
             std::optional<std::uint64_t> handle = std::nullopt);
    EntityCommon commonOf(const Record& r);
    bool frameOf(const Record& r, Frame& f, std::string_view type);
    LayerId layerFor(std::string_view raw, int line);
    LinetypeId linetypeFor(std::string_view raw, int line);
    TextStyleId textStyleFor(std::string_view raw) const;
    DimStyleId dimStyleFor(std::string_view raw) const;
    DimStyleId dimStyleWithOverrides(const Record& r, DimStyleId base);
    std::map<std::string, DimStyleId> dimOverrideStyles_{};
    // Width factor a STYLE record states, by style id (before any stand-in widening): the TEXT
    // group 41 is the object's full factor, TBCAD stores it relative to its style's.
    std::map<std::uint32_t, double> styleRawWidth_{};
    std::map<std::string, double> styleHeights_{};  // STYLE handle -> fixed text height (DXF units)
    // Linetype text marks name their style by handle, and the LTYPE table comes before STYLE.
    std::map<std::string, std::string> styleNameByHandle_{};
    std::map<std::string, std::string> shapeFileByHandle_{};  // shape-file STYLE records
    std::vector<LinetypeId> marksAwaitingStyles_{};
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
    bool convMLeader(const Record& r, std::vector<Entity>& out);
    bool conv3dFace(const Record& r, std::vector<Entity>& out);
    bool convMLine(const Record& r, std::vector<Entity>& out);
    void loadMlineStyles();
    void importMlineStyles();
    std::unordered_map<std::string, mline::Style> mlStyles_{};  // by upper-case name
    bool mlStylesLoaded_ = false;
    bool convWipeout(const Record& r, std::vector<Entity>& out);
    bool convImage(const Record& r, std::vector<Entity>& out);
    bool convTolerance(const Record& r, std::vector<Entity>& out);
    int toleranceBlocks_ = 0;
    void resolveImages();
    bool convArcDimension(const Record& r, std::vector<Entity>& out);
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
    const GroupBuffer* groups_ = nullptr;
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
    // AutoCAD keeps layer transparency in the layer's extension dictionary (an XRECORD named
    // ADSK_XREC_LAYER_ALPHA): layer name -> dictionary handle, dictionary handle -> entries,
    // XRECORD handle -> its first number.
    std::vector<std::pair<std::string, std::string>> layerXdicts_{};
    std::unordered_map<std::string, std::vector<std::pair<std::string, std::string>>> dicts_{};
    std::unordered_map<std::string, std::int64_t> xrecNumbers_{};
    // AutoCAD annotative scales: SCALE object handle -> drawing units per paper unit, and
    // object-context-data handle -> the handle of the SCALE it is for.
    std::unordered_map<std::string, double> scaleFactors_{};
    std::unordered_map<std::string, std::string> contextScale_{};
    // Items of every XRECORD by handle, for extension dictionaries of entities.
    std::unordered_map<std::string, std::vector<DataItem>> xrecItems_{};
    // Every OBJECTS record as read (groups as text), for the part that is kept as it is.
    struct ObjRec {
        std::string type{};
        std::vector<std::pair<int, std::string>> g{};
    };
    std::unordered_map<std::string, ObjRec> objStore_{};
    std::size_t objStoreBytes_ = 0;
    std::string layerTableDict_{};  // extension dictionary of the LAYER table (layer filters)
    std::string objRoot_{};         // handle of the root dictionary
    // (entity id, handle in the source file) of the entities added so far
    std::vector<std::pair<std::uint64_t, std::uint64_t>> srcHandles_{};
    // Named plot styles: name of each plot style object by handle, and the entities (source
    // handle) that name one.
    std::unordered_map<std::string, std::string> plotStyleNames_{};
    std::vector<std::pair<std::uint64_t, std::string>> entityPlotStyles_{};
    ExtData extOf(const Record& r);
    void resolveExtDictionaries();
    ScaleList annotativeScalesOf(const std::vector<std::pair<std::string, std::string>>& xd);
    void applyLayerAlphaXrecords();
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
    bool sheetBlock_ = false;  // reading the objects of a layout sheet
    std::unordered_map<std::string, BlockId> blockByFold_{};
    std::unordered_set<BlockId> blockDefined_{};

    // paper space: every *Paper_Space block becomes a layout sheet (see libs/layout/layout.h)
    struct SheetViewport {
        Vec2 min{}, max{}, center{};
        double scaleN = 1;
        bool on = true;
        int id = 0;
        std::vector<std::string> frozen{};  // layer names (DXF spelling)
        std::vector<Vec2> clip{};           // non-rectangular outline on the sheet (mm)
        std::string layer{};                // group 8: the border plots with this layer
        std::string clipHandle{};           // group 340: the boundary polyline, found at the end
        std::string level;                  // TBCAD XDATA "vlevel=<name>"
        bool locked = false;                // TBCAD XDATA "locked=1"
        int border = -1;                    // TBCAD XDATA "border=0/1"; -1 = from the layer
    };
    struct Sheet {
        std::string foldedName{};
        BlockId content{};
        std::vector<SheetViewport> viewports{};
        double extentX = 0, extentY = 0;  // of the drawn objects, for a size when none is stored
    };
    struct LayoutRow {
        std::string name{};
        int tab = 0;
        double width = 0, height = 0;  // mm as stored (0 = unknown)
        int rotation = 0;
        std::string styleSheet{};  // plot style table (group 7)
        bool hasExtents = false;   // the paper rectangle in paper space coordinates
        Vec2 extMin{}, extMax{};
    };
    std::vector<Sheet> sheets_{};
    std::vector<LayoutRow> layoutRows_{};
    int curSheet_ = -1;  // the sheet whose block is being read (BLOCKS section)

    // diagnostics accumulated for one summary line each
    std::map<std::string, std::pair<std::size_t, int>> skippedTypes_{};  // type -> (count, line)
    // IMAGE entities become picture blocks (see libs/underlay); the file name comes from the
    // IMAGEDEF object, which the OBJECTS section holds after the entities, so it is filled in last.
    struct PendingImage {
        BlockId block{};
        std::string name{};
        std::string defHandle{};
        double fade = 0;
        double u0 = 0, v0 = 0, u1 = 1, v1 = 1;
        int line = 0;
    };
    std::vector<PendingImage> images_{};
    std::map<std::string, std::vector<Vec2>>
        sheetRings_{};                                // closed polylines on sheets, by handle
    std::map<std::string, std::string> imageDefs_{};  // IMAGEDEF handle -> file path
    std::map<std::string, int> imageNames_{};
    std::map<char, int> mtextDropped_{};
    std::size_t flattened_ = 0;
    std::size_t paperSpace_ = 0;
    int firstFlattenedLine_ = 0;
    std::size_t mirroredText_ = 0;
    int firstMirroredLine_ = 0;
};

}  // namespace tbcad::dxf::detail
