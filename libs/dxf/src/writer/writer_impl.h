// Internal: the Document -> DXF exporter. Public entry point is exportDxf() in writer.h.
#pragma once

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "colors.h"
#include "tbcad/core/document.h"
#include "tbcad/dxf/writer.h"
#include "tbcad/gdt/gdt.h"

namespace tbcad::dxf::detail {

// Group-code level output.
class Out {
public:
    Out(bool utf8, bool r12) : utf8_(utf8), r12_(r12) {}

    std::string buf{};
    std::size_t badNumbers = 0;

    // The XDATA application the record is in (the last 1001 group since its "0" group).
    std::string xdataApp{};

    void raw(int code, std::string_view ascii) {
        if (code == 1001)
            xdataApp = ascii;
        emit(code, ascii.data(), ascii.size());
    }
    void str(int code, std::string_view utf8) { raw(code, encodeText(utf8, utf8_)); }
    void i(int code, long long v) {
        char b[24];
        const auto r = std::to_chars(b, b + sizeof b, v);
        emit(code, b, static_cast<std::size_t>(r.ptr - b));
    }
    void d(int code, double v) {
        if (!std::isfinite(v)) {
            ++badNumbers;
            v = 0;
        }
        // The shortest text that reads back as the same number, with ".0" when it looks whole.
        char b[48];
        const auto r = std::to_chars(b, b + 44, v);
        char* end = r.ptr;
        bool plain = true;
        for (const char* c = b; c != end; ++c)
            if (*c == '.' || *c == 'e' || *c == 'E' || *c == 'n') {
                plain = false;
                break;
            }
        if (plain) {
            *end++ = '.';
            *end++ = '0';
        }
        emit(code, b, static_cast<std::size_t>(end - b));
    }
    void h(int code, std::uint64_t v) {
        char b[24];
        const auto r = std::to_chars(b, b + sizeof b, v, 16);
        for (char* c = b; c != r.ptr; ++c)
            if (*c >= 'a' && *c <= 'f')
                *c = static_cast<char>(*c - 'a' + 'A');
        emit(code, b, static_cast<std::size_t>(r.ptr - b));
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
    void zero(std::string_view type) {
        xdataApp.clear();
        raw(0, type);
    }

    bool r12() const { return r12_; }
    bool utf8() const { return utf8_; }

private:
    // "<code>\r\n<value>\r\n" with one growth check.
    void emit(int code, const char* value, std::size_t n) {
        char c[16];
        const auto r = std::to_chars(c, c + 12, code);
        const std::size_t cl = static_cast<std::size_t>(r.ptr - c);
        c[cl] = '\r';
        c[cl + 1] = '\n';
        buf.append(c, cl + 2);
        buf.append(value, n);
        buf.append("\r\n", 2);
    }

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
    std::string plotStyle{};                        // named plot style of the object itself, if any
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
    // New handles skip the ones entities keep from the source file.
    std::uint64_t newHandle() {
        while (srcHandleReserved_.count(L().nextHandle))
            ++L().nextHandle;
        return L().nextHandle++;
    }
    std::uint64_t peekHandle() {
        while (srcHandleReserved_.count(L().nextHandle))
            ++L().nextHandle;
        return L().nextHandle;
    }
    std::uint64_t handleSeed() const { return std::max(L().nextHandle, srcHandleTop_ + 1); }
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
    std::uint64_t layerTable_ = 0, layerTableDict_ = 0;  // LAYER table and its dictionary
    void writeLinetypes(Out& o, std::uint64_t table);
    void writeStyles(Out& o, std::uint64_t table);
    void writeMark(Out& o, const LinetypeMark& m);
    void writeDimStyles(Out& o, std::uint64_t table);
    void writeBlockRecords(Out& o, std::uint64_t table);
    void blockBegin(Out& o, const std::string& name, std::uint64_t rec, Vec2 base, int flags,
                    bool paperSpace, const BlockDef* xref = nullptr);
    void blockEnd(Out& o, std::uint64_t rec, bool paperSpace);

    // ---- entities.cpp --------------------------------------------------------------------
    Props propsOf(const EntityCommon& c);
    // The handle of the plot style name object for `name` (made on first use; written with the
    // plot style dictionary in the OBJECTS section).
    std::uint64_t pstyleHandle(const std::string& name);
    std::vector<std::pair<std::string, std::uint64_t>> pstyles_{};
    void colorGroups(const Props& p);
    void head(std::string_view type, const Props& p, std::uint64_t owner,
              std::string_view subclass);
    void sub(std::string_view subclass);
    void count(std::string_view type, std::size_t n = 1) {
        emitLevelTag();
        auto& counts = L().counts;
        auto it = counts.find(type);
        if (it == counts.end())
            it = counts.emplace(std::string(type), 0).first;
        it->second += n;
    }
    // TBCAD XDATA naming the entity's level ("level=<name>"), written at the end of the
    // record that setLevelTag() prepared; a no-op when no tag is pending.
    void emitLevelTag();
    // Opens the record's TBCAD XDATA unless it is the application being written (one 1001 per
    // application and record).
    void openTbcadXdata();
    // TBCAD XDATA flags every kind of object can carry: wipeout, annotative and its scales.
    void writeCommonFlags(const EntityCommon& c);
    // TBCAD_DIM XDATA: what a dimension or leader has beyond its DXF record (DIMBREAK gaps and
    // the objects that make them, jogs, flipped arrows, associative points); the objects named
    // are given handles before any is written (see loadSourceHandles).
    void writeDimExtras(const std::vector<DimBreak>& breaks, const std::vector<EntityId>& with,
                        std::uint32_t autoBreaks, const DimensionData* dim,
                        const LeaderData* leader = nullptr);
    // A leader without text keeps its text point (where text added later goes) only here: DXF
    // reads it back at the last vertex.
    static bool leaderKeepsTextPos(const LeaderData& l) {
        return l.utf8.empty() && !l.vertices.empty() && l.textPos != l.vertices.back();
    }
    std::string handleRef(EntityId id) const;
    void reserveNamedHandles();
    void setLevelTag(const EntityCommon& c);
    void levelsXdata(Out& o);
    void skipEntity(std::string_view what, const std::string& why);
    void downgrade(std::string_view what, const std::string& why);

    std::uint64_t layerHandleOf(LayerId id);
    void writeEntityList(const std::vector<EntityId>& ids, std::uint64_t owner);
    // True for an object whose record is written from the object alone: the only thing it takes
    // from the writer is one handle, so many such records can be written on several threads once
    // their handles are fixed (see writeEntityList).
    bool plainEntity(const Entity& e);
    bool plainBlock(BlockId id);
    std::unordered_map<std::uint32_t, bool> plainBlock_{};
    const gdt::FrameTable& frames();
    std::optional<gdt::FrameTable> frames_{};
    struct PlainItem {
        const Entity* e = nullptr;
        std::uint64_t kept = 0;   // the handle it came with, or 0
        std::uint64_t fresh = 0;  // the handle it takes when it came with none
    };
    struct PlainUnit {
        std::size_t at = 0;            // where in the output the unit's records go
        std::size_t from = 0, to = 0;  // PlainItems
    };
    // Writes the deferred plain records (on several threads) and puts them in place.
    void flushPlain(std::vector<PlainItem>& items, std::vector<PlainUnit>& units,
                    std::uint64_t owner);
    void writeEntity(const Entity& e, std::uint64_t owner);
    void writeDisplayList(const DisplayList& list, const std::string& fallbackLayer,
                          std::uint64_t owner);
    Props propsOfStyle(const DisplayStyle& st, const std::string& fallbackLayer);

    void writeLine(const Props& p, const LineData& d, std::uint64_t owner);
    void writeCircle(const Props& p, const CircleData& d, std::uint64_t owner);
    void writeArc(const Props& p, const ArcData& d, std::uint64_t owner);
    void writePolyline(const Props& p, const PolylineData& d, std::uint64_t owner);
    bool writeWipeout(const Props& p, const PolylineData& d, std::uint64_t owner);
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
    // A PDF page block (PDFATTACH, or a PDFUNDERLAY read from DXF) is written as PDFUNDERLAY +
    // PDFDEFINITION.
    bool writePdfUnderlay(const Props& p, const InsertData& d, std::uint64_t owner);
    bool writeTolerance(const Props& p, const InsertData& d, std::uint64_t owner);
    void writeLeader(const Props& p, const LeaderData& d, std::uint64_t owner);
    void writeTable(const Props& p, const Entity& e, std::uint64_t owner);
    void writeRay(const Props& p, const RayData& d, const Entity& e, std::uint64_t owner);
    void writeMline(const Props& p, const MlineData& d, const Entity& e, std::uint64_t owner);
    void writeAnnotativeData(const Entity& e);
    void mleaderNote(const MLeaderData& m, std::vector<DataItem>& items);
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

    // Handles entities had in the source file (entity id -> handle), each handle once.
    std::unordered_map<std::uint64_t, std::uint64_t> srcHandle_{};
    std::unordered_set<std::uint64_t> srcHandleReserved_{};
    std::uint64_t srcHandleTop_ = 0;
    std::unordered_set<std::uint64_t> groupMembers_{};                // entities named by a group
    std::unordered_map<std::uint64_t, std::uint64_t> groupHandle_{};  // their handles
    void loadSourceHandles();
    void pickEntityHandle(EntityId id);
    struct ImageOut {
        std::string name{};  // key in ACAD_IMAGE_DICT
        std::string path{};
        double w = 0, h = 0;
        std::uint64_t def = 0, reactor = 0, image = 0;
        bool frame = true;  // the frame of the picture is shown
    };
    std::vector<ImageOut> images_{};
    bool wipeouts_ = false;  // a WIPEOUT was written: it needs its class and variables
    struct PdfOut {
        std::string name{};  // key in ACAD_PDFDEFINITIONS
        std::string file{};
        std::string page{};
        std::uint64_t def = 0;
    };
    std::vector<PdfOut> pdfs_{};
    ExportSummary summary_{};

    // Handles given to text styles before their table is written (linetype marks refer to them).
    std::unordered_map<std::uint32_t, std::uint64_t> tsHandle_{};
    std::map<std::string, std::uint64_t> shapeFileHandle_{};  // shape files named by linetypes
    std::unordered_map<std::uint32_t, std::uint64_t> layerHandle_{};  // LAYER record handles
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

    // The state that changes while entities are written. The entities of a big plain run are
    // written on several threads, each with a Lane of its own (see writeParallelRun); L() is the
    // calling thread's.
    struct Lane {
        Out* o = nullptr;
        std::uint64_t nextHandle = 0x20;
        std::uint64_t entHandle = 0;  // the handle the next head() gives the entity, 0 = a new one
        std::uint64_t lastHead = 0;   // the handle the last head() wrote
        std::string levelTag{};
        bool tbcadOpen = false;             // the record's TBCAD XDATA has begun (head() clears it)
        const EntityCommon* cur = nullptr;  // the entity writeEntity is on
        std::vector<PendingDict> pendingDicts{};
        std::unordered_set<std::uint64_t> handlesWritten{};
        std::map<std::string, std::size_t, std::less<>> counts{};  // DXF type name -> records
    };
    Lane main_{};
    static thread_local Lane* tlLane_;
    Lane& L() { return tlLane_ ? *tlLane_ : main_; }
    const Lane& L() const { return tlLane_ ? *tlLane_ : main_; }
    // Set while the items of an associative array are written: the TBCAD_ARRAY XDATA that lets
    // the importer fuse them into one array again.
    std::vector<DataItem> arrayTag_{};
    int nextArrayId_ = 1;
    int nextMleaderId_ = 1;
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
    // False when no object has a plot style of its own, so the per-object lookup can be skipped.
    bool anyEntityPlotStyle_ = true;
    std::unordered_map<std::string, std::vector<HatchPatternLine>> patternCache_{};
};

}  // namespace tbcad::dxf::detail
