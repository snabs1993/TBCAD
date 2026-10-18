// Internal: the Document -> DXF exporter. Public entry point is exportDxf() in writer.h.
#pragma once

#include <cmath>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "colors.h"
#include "tbcad/core/document.h"
#include "tbcad/dxf/writer.h"

namespace tbcad::dxf::detail {

// Group-code level output.
class Out {
public:
    Out(bool utf8, bool r12) : utf8_(utf8), r12_(r12) {}

    std::string buf{};
    std::size_t badNumbers = 0;

    void raw(int code, std::string_view ascii) {
        buf += std::to_string(code);
        buf += "\r\n";
        buf += ascii;
        buf += "\r\n";
    }
    void str(int code, std::string_view utf8) { raw(code, encodeText(utf8, utf8_)); }
    void i(int code, long long v) { raw(code, std::to_string(v)); }
    void d(int code, double v) {
        if (!std::isfinite(v)) {
            ++badNumbers;
            v = 0;
        }
        raw(code, formatNumber(v));
    }
    void h(int code, std::uint64_t v) {
        static const char* digits = "0123456789ABCDEF";
        std::string s;
        do {
            s.insert(s.begin(), digits[v & 15]);
            v >>= 4;
        } while (v);
        raw(code, s);
    }
    // x at `code`, y at code+10, z at code+20.
    void pt(int code, Vec2 p, double z = 0) {
        d(code, p.x);
        d(code + 10, p.y);
        d(code + 20, z);
    }
    // 2D point without a z group (LWPOLYLINE vertices, HATCH points).
    void pt2(int code, Vec2 p) {
        d(code, p.x);
        d(code + 10, p.y);
    }
    // Section / table / entity type: "0 <type>".
    void zero(std::string_view type) { raw(0, type); }

    bool r12() const { return r12_; }
    bool utf8() const { return utf8_; }

private:
    bool utf8_;
    bool r12_;
};

// Case-folded name set used to keep DXF names unique.
class NameSet {
public:
    bool has(const std::string& name) const { return used_.count(fold(name)) != 0; }
    void add(const std::string& name) { used_.insert(fold(name)); }
    static std::string fold(std::string s) {
        for (char& c : s) {
            if (c >= 'A' && c <= 'Z')
                c = static_cast<char>(c - 'A' + 'a');
        }
        return s;
    }

private:
    std::unordered_set<std::string> used_{};
};

// The width factor a STYLE record is written with (the stroke stand-in's widening is undone).
double styleWidthWritten(const TextStyle& ts);

// Group 440 for a transparency in percent (0x02000000 | alpha) or ByBlock (0x01000000).
inline std::int64_t transparencyGroup(int percent) {
    if (percent == kTransparencyByBlock)
        return 0x01000000;
    const long alpha = std::lround(255.0 * (100 - percent) / 100.0);
    return 0x02000000 | alpha;
}

// What every entity record starts with.
struct Props {
    std::string layer = "0";
    std::string linetype{};  // empty = ByLayer (group omitted)
    Color color = Color::byLayer();
    std::optional<Rgba> trueColor{};  // exact colour of a resolved display style
    LineWeight lineweight = LineWeight::byLayer();
    std::int8_t transparency = kTransparencyByLayer;
    double ltScale = 1;
    bool visible = true;
    const std::vector<DictRecord>* dict = nullptr;  // extension dictionary records, if any
};

class Writer {
public:
    Writer(const Document& doc, const ExportOptions& options, Report& report);
    Result<ExportResult> run();

private:
    friend struct HatchWriter;

    // ---- writer.cpp: names, tables, sections ---------------------------------------------
    bool r12() const { return r12_; }
    std::uint64_t newHandle() { return nextHandle_++; }
    std::string cleanName(const std::string& name, const char* what);
    std::string uniqueName(NameSet& set, const std::string& base, const char* what);
    void prepareNames();
    void prepareBlocks();
    std::string layerPath(LayerId id) const;

    std::string layerNameOf(LayerId id) const;
    std::string linetypeNameOf(LinetypeId id) const;  // empty for ByLayer
    std::string textStyleNameOf(TextStyleId id) const;
    std::string dimStyleNameOf(DimStyleId id) const;
    std::string blockNameOf(BlockId id) const;

    void tableBegin(Out& o, std::string_view name, std::uint64_t handle, std::size_t count);
    void tableEnd(Out& o);
    void tableRecord(Out& o, std::string_view type, std::uint64_t handle, std::uint64_t owner,
                     std::string_view subclass, std::string_view name, int flags);
    std::string headerSection(std::uint64_t handSeed);
    std::string tablesSection();
    std::string objectsSection();
    void writeLayers(Out& o, std::uint64_t table);
    void writeLinetypes(Out& o, std::uint64_t table);
    void writeStyles(Out& o, std::uint64_t table);
    void writeDimStyles(Out& o, std::uint64_t table);
    void writeBlockRecords(Out& o, std::uint64_t table);
    void blockBegin(Out& o, const std::string& name, std::uint64_t rec, Vec2 base, int flags,
                    bool paperSpace, const BlockDef* xref = nullptr);
    void blockEnd(Out& o, std::uint64_t rec, bool paperSpace);

    // ---- entities.cpp --------------------------------------------------------------------
    Props propsOf(const EntityCommon& c);
    void colorGroups(const Props& p);
    void head(std::string_view type, const Props& p, std::uint64_t owner,
              std::string_view subclass);
    void sub(std::string_view subclass);
    void count(std::string_view type, std::size_t n = 1) {
        emitLevelTag();
        summary_.entities[std::string(type)] += n;
    }
    // TBCAD XDATA naming the entity's level ("level=<name>"), written at the end of the
    // record that setLevelTag() prepared; a no-op when no tag is pending.
    void emitLevelTag();
    void setLevelTag(const EntityCommon& c);
    void levelsXdata(Out& o);
    void skipEntity(std::string_view what, const std::string& why);
    void downgrade(std::string_view what, const std::string& why);

    void writeEntityList(const std::vector<EntityId>& ids, std::uint64_t owner);
    void writeEntity(const Entity& e, std::uint64_t owner);
    void writeDisplayList(const DisplayList& list, const std::string& fallbackLayer,
                          std::uint64_t owner);
    Props propsOfStyle(const DisplayStyle& st, const std::string& fallbackLayer);

    void writeLine(const Props& p, const LineData& d, std::uint64_t owner);
    void writeCircle(const Props& p, const CircleData& d, std::uint64_t owner);
    void writeArc(const Props& p, const ArcData& d, std::uint64_t owner);
    void writePolyline(const Props& p, const PolylineData& d, std::uint64_t owner);
    void writePoint(const Props& p, const PointData& d, std::uint64_t owner);
    void writeEllipse(const Props& p, const EllipseData& d, const Entity& e, std::uint64_t owner);
    void writeSpline(const Props& p, const SplineData& d, const Entity& e, std::uint64_t owner);
    void writeText(const Props& p, const TextData& d, std::uint64_t owner);
    void writeTextRecord(const Props& p, const TextData& d, std::uint64_t owner,
                         std::string_view type, const std::string& shown);
    void writeMText(const Props& p, const MTextData& d, std::uint64_t owner);
    void writeInsert(const Props& p, const InsertData& d, std::uint64_t owner);
    // A picture block (IMAGEATTACH, or an IMAGE read from DXF) is written as IMAGE + IMAGEDEF.
    bool writeImage(const Props& p, const InsertData& d, std::uint64_t owner);
    bool writeTolerance(const Props& p, const InsertData& d, std::uint64_t owner);
    void writeLeader(const Props& p, const LeaderData& d, std::uint64_t owner);
    void writeTable(const Props& p, const Entity& e, std::uint64_t owner);
    void writeRay(const Props& p, const RayData& d, const Entity& e, std::uint64_t owner);
    void writeMline(const Props& p, const MlineData& d, const Entity& e, std::uint64_t owner);
    void writeAnnotativeData(const Entity& e);
    void writeExtXdata(const EntityCommon& c);
    void writeTbcadFlags(const Entity& e);
    void writeProxy(const Props& p, const ProxyData& d, std::uint64_t owner);
    void writeSolid(const Props& p, const std::vector<Vec2>& ring, std::uint64_t owner);
    void writeCurveFallback(const Entity& e, const Props& p, std::uint64_t owner);
    void writeVertexPolyline(const Props& p, const std::vector<PolyVertex>& v, bool closed,
                             double constantWidth, std::uint64_t owner, bool runsOn = true);
    // Writes the dimension at ids[index] and returns how many ids it used (>= 1).
    std::size_t writeDimension(const std::vector<EntityId>& ids, std::size_t index,
                               std::uint64_t owner);

    // ---- hatch.cpp -----------------------------------------------------------------------
    void writeHatch(const Props& p, const HatchData& d, std::uint64_t owner);
    void writeHatchBoundaryAsLines(const Props& p, const HatchData& d, std::uint64_t owner);
    void writeSolidHatch(const Props& p, const std::vector<std::vector<Vec2>>& rings,
                         std::uint64_t owner);
    std::optional<std::vector<HatchPatternLine>> patternLines(const std::string& name);

    // ---- state ---------------------------------------------------------------------------
    const Document& doc_;
    ExportOptions opt_;
    Report& rep_;
    bool r12_;
    bool utf8_;
    std::string layerSep_;

    std::uint64_t nextHandle_ = 0x20;
    struct ImageOut {
        std::string name{};  // key in ACAD_IMAGE_DICT
        std::string path{};
        double w = 0, h = 0;
        std::uint64_t def = 0, reactor = 0, image = 0;
    };
    std::vector<ImageOut> images_{};
    Out* o_ = nullptr;
    ExportSummary summary_{};

    std::unordered_map<std::uint32_t, std::string> layerName_{}, ltName_{}, tsName_{}, dsName_{},
        blockName_{};
    NameSet layerSet_{}, ltSet_{}, tsSet_{}, dsSet_{}, blockSet_{};
    std::unordered_map<std::uint32_t, std::uint64_t> blockRec_{};
    std::unordered_map<const std::vector<double>*, std::string> ltByPattern_{};

    std::uint64_t modelRec_ = 0, paperRec_ = 0;
    std::uint64_t rootDict_ = 0;
    // Extension dictionaries of entities, written to OBJECTS once all entities are out.
    struct PendingDict {
        std::uint64_t handle = 0, owner = 0;
        std::vector<DictRecord> records{};
    };
    std::vector<PendingDict> pendingDicts_{};
    const EntityCommon* curCommon_ = nullptr;  // the entity writeEntity is on
    std::uint64_t layoutModel_ = 0, layoutPaper_ = 0;
    std::vector<BlockId> userBlocks_{};  // blocks written to BLOCKS, in id order

    // Paper-space layouts (the table in Header::custom["tbcad.layouts"], see libs/layout): the
    // first is written as *Paper_Space, the others as *Paper_Space0, *Paper_Space1, ...
    struct SheetViewport {
        int id = 0;
        Vec2 min{}, max{}, center{};
        double scaleN = 1;
        bool on = true;
        std::vector<LayerId> frozen{};
        std::vector<Vec2> clip{};  // outline of a clipped viewport (sheet mm)
        bool locked = false;
        bool border = true;
        std::uint32_t level = 0;
    };
    struct Sheet {
        std::string name{};
        BlockId content{};
        double w = 420, h = 297;
        std::uint64_t rec = 0, layout = 0;
        std::vector<SheetViewport> viewports{};
    };
    std::vector<Sheet> sheets_{};
    void prepareSheets();
    void writeSheetBlock(Out& o, const Sheet& sheet, const std::string& blockName);

    struct DimBlock {
        std::string name{};
        std::uint64_t rec = 0;
        std::string body{};
    };
    std::vector<DimBlock> dimBlocks_{};
    int dimCounter_ = 0;

    std::unordered_set<std::string> warnedOnce_{};
    std::unordered_set<std::string> warnedPatterns_{};
    BBox2 extents_{};
    bool r12HatchWarned_ = false;
    bool tagLevels_ = false;  // the document has more than the pristine default level
    std::string levelTag_{};
    std::unordered_map<std::string, std::vector<HatchPatternLine>> patternCache_{};
};

}  // namespace tbcad::dxf::detail
