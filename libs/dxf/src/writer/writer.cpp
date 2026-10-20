// DXF export: file structure (header, tables, blocks, objects) and names.
#include <algorithm>
#include <cctype>
#include <cmath>
#include <functional>
#include <future>
#include <set>
#include <sstream>
#include <thread>

#include "keep.h"
#include "tbcad/gdt/gdt.h"
#include "tbcad/geom/entity.h"
#include "tbcad/mline/mline.h"
#include "tbcad/text/text.h"
#include "tbcad/underlay/image.h"
#include "tbcad/underlay/pdf.h"
#include "writer_impl.h"

namespace tbcad::dxf::detail {

thread_local Writer::Lane* Writer::tlLane_ = nullptr;

namespace {

constexpr double kDeg = 180.0 / kPi;

bool isInvalidNameChar(char c) {
    switch (c) {
        case '<':
        case '>':
        case '/':
        case '\\':
        case '"':
        case ':':
        case ';':
        case '?':
        case '*':
        case '|':
        case '=':
        case '`':
            return true;
        default:
            return static_cast<unsigned char>(c) < 0x20;
    }
}

}  // namespace

Writer::Writer(const Document& doc, const ExportOptions& options, Report& report)
    : doc_(doc),
      opt_(options),
      rep_(report),
      r12_(options.version == DxfVersion::R12),
      utf8_(options.version == DxfVersion::R2007),
      layerSep_(options.layerSeparator) {
    const Level* first = doc.level(kLevelDefault);
    tagLevels_ =
        !options.onlyLevel &&
        (doc.levels().size() > 1 || (first && (first->name != "Level 1" || first->elevation != 0 ||
                                               !first->visible || first->locked)));
}

// ---------------------------------------------------------------------------------------------
// Names

std::string Writer::cleanName(const std::string& name, const char* what) {
    std::string out;
    bool leadingStar = !name.empty() && name[0] == '*' && std::string_view(what) == "block";
    for (std::size_t i = 0; i < name.size(); ++i) {
        char c = name[i];
        if (i == 0 && leadingStar) {
            out.push_back(c);
        } else if (isInvalidNameChar(c)) {
            out.push_back('_');
        } else {
            out.push_back(c);
        }
    }
    // Spaces at the ends are dropped by most readers.
    while (!out.empty() && out.back() == ' ')
        out.pop_back();
    std::size_t start = 0;
    while (start < out.size() && out[start] == ' ')
        ++start;
    out.erase(0, start);
    if (out.empty())
        out = "_";
    if (r12_ && out.size() > 31) {
        // Cut on a character boundary.
        std::size_t cut = 31;
        while (cut > 0 && (static_cast<unsigned char>(out[cut]) & 0xC0) == 0x80)
            --cut;
        out.resize(cut);
    }
    if (out != name) {
        rep_.info(wcode::kNameChanged, std::string(what) + " name \"" + name +
                                           "\" is not valid in DXF; written as \"" + out + "\"");
    }
    return out;
}

std::string Writer::uniqueName(NameSet& set, const std::string& base, const char* what) {
    std::string name = base;
    int n = 2;
    while (set.has(name)) {
        std::string suffix = "_" + std::to_string(n++);
        name = base;
        if (r12_ && name.size() + suffix.size() > 31)
            name.resize(31 - suffix.size());
        name += suffix;
    }
    if (name != base) {
        rep_.info(wcode::kNameChanged, std::string(what) + " name \"" + base +
                                           "\" is already used in the DXF file; written as \"" +
                                           name + "\"");
    }
    set.add(name);
    return name;
}

std::string Writer::layerPath(LayerId id) const {
    std::vector<std::string> parts;
    for (LayerId p = id; p;) {
        const Layer* l = doc_.layer(p);
        if (!l)
            break;
        parts.push_back(l->name);
        p = l->parent;
        if (parts.size() > 64)
            break;
    }
    std::string path;
    for (auto it = parts.rbegin(); it != parts.rend(); ++it) {
        if (!path.empty())
            path += layerSep_;
        path += *it;
    }
    return path;
}

void Writer::prepareNames() {
    // Layers. Parents have smaller ids than their children, so the map order is a good order.
    layerSet_.add("0");
    layerName_[kLayerZero.value] = "0";
    for (const auto& [id, layer] : doc_.layers()) {
        if (id == kLayerZero)
            continue;
        std::string base = cleanName(layerPath(id), "layer");
        layerName_[id.value] = uniqueName(layerSet_, base, "layer");
    }

    // Linetypes.
    ltSet_.add("BYLAYER");
    ltSet_.add("BYBLOCK");
    ltSet_.add("CONTINUOUS");
    ltName_[kLinetypeContinuous.value] = "CONTINUOUS";
    for (const auto& [id, lt] : doc_.linetypes()) {
        if (id == kLinetypeContinuous)
            continue;
        ltName_[id.value] = uniqueName(ltSet_, cleanName(lt.name, "linetype"), "linetype");
        if (!lt.pattern.empty())
            ltByPattern_[&lt.pattern] = ltName_[id.value];
    }

    for (const auto& [id, ts] : doc_.textStyles())
        tsName_[id.value] = uniqueName(tsSet_, cleanName(ts.name, "text style"), "text style");
    for (const auto& [id, ds] : doc_.dimStyles())
        dsName_[id.value] = uniqueName(dsSet_, cleanName(ds.name, "dimension style"), "dim style");
}

void Writer::prepareBlocks() {
    blockSet_.add("*Model_Space");
    blockSet_.add("*Paper_Space");
    std::unordered_set<std::uint32_t> referenced;
    doc_.forEachEntity([&](const Entity& e) {
        if (const auto* ins = entityAs<InsertData>(e))
            referenced.insert(ins->block.value);
    });
    const gdt::FrameTable frames = gdt::readFrames(doc_.header());
    for (const auto& [id, def] : doc_.blocks()) {
        bool anon = def.anonymous || (!def.name.empty() && def.name[0] == '*');
        bool dimBlock = anon && def.name.size() >= 2 && def.name[0] == '*' &&
                        (def.name[1] == 'D' || def.name[1] == 'd');
        // Dimension blocks are regenerated from the dimension's graphics (see writeDimension).
        if (dimBlock && !referenced.count(id.value))
            continue;
        // A picture is written as an IMAGE entity; its block has no DXF form.
        if (!def.xrefPath.empty() &&
            (underlay::isImagePath(def.xrefPath) || underlay::isPdfPath(def.xrefPath)))
            continue;
        // So is a tolerance frame, as a TOLERANCE.
        if (frames.count(def.name))
            continue;
        std::string name = cleanName(def.name, "block");
        if (anon && name[0] != '*')
            name = "*" + name;
        blockName_[id.value] = uniqueName(blockSet_, name, "block");
        blockRec_[id.value] = newHandle();
        userBlocks_.push_back(id);
    }
}

namespace {

std::vector<std::string> splitOn(const std::string& s, char sep) {
    std::vector<std::string> out;
    std::size_t i = 0;
    for (;;) {
        const std::size_t j = s.find(sep, i);
        if (j == std::string::npos) {
            out.push_back(s.substr(i));
            return out;
        }
        out.push_back(s.substr(i, j - i));
        i = j + 1;
    }
}

}  // namespace

void Writer::prepareSheets() {
    const auto it = doc_.header().custom.find("tbcad.layouts");
    if (it == doc_.header().custom.end())
        return;
    for (const std::string& line : splitOn(it->second, '\n')) {
        const std::vector<std::string> f = splitOn(line, '\t');
        if (f.size() >= 8 && f[0] == "L") {
            Sheet s;
            s.name = f[2];
            s.w = std::strtod(f[5].c_str(), nullptr);
            s.h = std::strtod(f[6].c_str(), nullptr);
            if (!(s.w > 0) || !(s.h > 0)) {
                s.w = 420;
                s.h = 297;
            }
            s.content =
                BlockId{static_cast<std::uint32_t>(std::strtoul(f[7].c_str(), nullptr, 10))};
            sheets_.push_back(std::move(s));
        } else if (f.size() >= 10 && f[0] == "V" && !sheets_.empty()) {
            SheetViewport v;
            v.id = std::atoi(f[1].c_str());
            v.min = {std::strtod(f[2].c_str(), nullptr), std::strtod(f[3].c_str(), nullptr)};
            v.max = {std::strtod(f[4].c_str(), nullptr), std::strtod(f[5].c_str(), nullptr)};
            v.center = {std::strtod(f[6].c_str(), nullptr), std::strtod(f[7].c_str(), nullptr)};
            v.scaleN = std::strtod(f[8].c_str(), nullptr);
            v.on = f[9] == "1";
            if (f.size() > 10 && !f[10].empty())
                for (const std::string& id : splitOn(f[10], ','))
                    v.frozen.push_back(
                        LayerId{static_cast<std::uint32_t>(std::strtoul(id.c_str(), nullptr, 10))});
            v.locked = f.size() > 12 && f[12] == "1";
            if (f.size() > 11)
                v.level = static_cast<std::uint32_t>(std::strtoul(f[11].c_str(), nullptr, 10));
            v.border = !(f.size() > 14 && f[14] == "0");
            if (f.size() > 13 && !f[13].empty()) {
                for (const std::string& pt : splitOn(f[13], ';')) {
                    const std::size_t c = pt.find(':');
                    if (c != std::string::npos)
                        v.clip.push_back({std::strtod(pt.substr(0, c).c_str(), nullptr),
                                          std::strtod(pt.substr(c + 1).c_str(), nullptr)});
                }
                if (v.clip.size() < 3)
                    v.clip.clear();
            }
            sheets_.back().viewports.push_back(std::move(v));
        }
    }
    // The sheets' own blocks are written as the paper space blocks, not as user blocks.
    for (const Sheet& s : sheets_)
        userBlocks_.erase(std::remove(userBlocks_.begin(), userBlocks_.end(), s.content),
                          userBlocks_.end());
    for (std::size_t i = 0; i < sheets_.size(); ++i) {
        Sheet& s = sheets_[i];
        if (i == 0) {
            s.rec = paperRec_;
            s.layout = layoutPaper_;
        } else {
            s.rec = newHandle();
            s.layout = newHandle();
            blockSet_.add("*Paper_Space" + std::to_string(i - 1));
        }
    }
}

void Writer::writeSheetBlock(Out& o, const Sheet& sheet, const std::string& blockName) {
    blockBegin(o, blockName, sheet.rec, {0, 0}, 0, true);
    if (const BlockDef* def = doc_.block(sheet.content)) {
        Out* saved = L().o;
        L().o = &o;
        writeEntityList(def->entities, sheet.rec);
        L().o = saved;
    }
    // The viewports: where the window sits on the paper (10/20, 40/41) and which model area it
    // shows (view centre 12/22, view height 45); the frozen layers are TBCAD XDATA.
    for (const SheetViewport& v : sheet.viewports) {
        const double w = v.max.x - v.min.x, h = v.max.y - v.min.y;
        o.zero("VIEWPORT");
        o.h(5, newHandle());
        o.h(330, sheet.rec);
        o.raw(100, "AcDbEntity");
        o.i(67, 1);
        o.raw(8, "0");
        o.raw(100, "AcDbViewport");
        o.pt(10, {(v.min.x + v.max.x) / 2, (v.min.y + v.max.y) / 2});
        o.d(40, w);
        o.d(41, h);
        o.i(68, v.on ? 1 : 0);
        o.i(69, v.id + 1);  // 1 is the sheet's own overall viewport
        // Status flags: always enabled (32768), zoom locking (16384), turned off (131072).
        o.i(90, 32768 | (v.locked ? 16384 : 0) | (v.on ? 0 : 131072));
        o.d(12, v.center.x);
        o.d(22, v.center.y);
        o.pt(16, {0, 0}, 1);
        o.pt(17, {0, 0}, 0);
        o.d(45, h * v.scaleN);
        // The layers frozen in this viewport, as other programs read them (the names are also
        // kept in the TBCAD XDATA below).
        if (!r12_)
            for (LayerId id : v.frozen)
                o.h(331, layerHandleOf(id));
        if (!v.frozen.empty() || !v.clip.empty() || v.locked || !v.border || v.level != 0) {
            o.raw(1001, "TBCAD");
            if (v.locked)
                o.str(1000, "locked=1");
            if (!v.border)
                o.str(1000, "border=0");
            if (v.level != 0)
                if (const Level* lv = doc_.level(LevelId{v.level}))
                    o.str(1000, "vlevel=" + lv->name);
            for (LayerId id : v.frozen)
                o.str(1000, "frozen=" + layerNameOf(id));
            if (!v.clip.empty()) {
                // No boundary object is written: the outline is kept here, other programs show
                // the rectangle.
                std::string text = "clip=";
                for (std::size_t i = 0; i < v.clip.size(); ++i)
                    text += (i ? ";" : "") + formatNumber(v.clip[i].x) + ":" +
                            formatNumber(v.clip[i].y);
                o.str(1000, text);
                downgrade("VIEWPORT",
                          "a polygonal outline is kept only in TBCAD XDATA; other programs "
                          "show the rectangle");
            }
        }
    }
    blockEnd(o, sheet.rec, true);
}

std::string Writer::layerNameOf(LayerId id) const {
    auto it = layerName_.find(id.value);
    if (it != layerName_.end())
        return it->second;
    auto cur = layerName_.find(doc_.header().currentLayer.value);
    return cur != layerName_.end() ? cur->second : "0";
}

std::string Writer::linetypeNameOf(LinetypeId id) const {
    if (id == kLinetypeByLayer)
        return {};
    if (id == kLinetypeByBlock)
        return "BYBLOCK";
    auto it = ltName_.find(id.value);
    return it == ltName_.end() ? std::string() : it->second;
}

std::string Writer::textStyleNameOf(TextStyleId id) const {
    if (!id)
        id = doc_.header().currentTextStyle;
    auto it = tsName_.find(id.value);
    if (it != tsName_.end())
        return it->second;
    auto std_ = tsName_.find(kTextStyleStandard.value);
    return std_ != tsName_.end() ? std_->second : "Standard";
}

std::string Writer::dimStyleNameOf(DimStyleId id) const {
    if (!id)
        id = doc_.header().currentDimStyle;
    auto it = dsName_.find(id.value);
    if (it != dsName_.end())
        return it->second;
    auto std_ = dsName_.find(kDimStyleStandard.value);
    return std_ != dsName_.end() ? std_->second : "Standard";
}

std::string Writer::blockNameOf(BlockId id) const {
    auto it = blockName_.find(id.value);
    return it == blockName_.end() ? std::string() : it->second;
}

// ---------------------------------------------------------------------------------------------
// Tables

void Writer::tableBegin(Out& o, std::string_view name, std::uint64_t handle, std::size_t count) {
    o.zero("TABLE");
    o.raw(2, name);
    if (!r12_) {
        o.h(5, handle);
        o.i(330, 0);
        if (name == "LAYER" && layerTableDict_) {
            o.raw(102, "{ACAD_XDICTIONARY");
            o.h(360, layerTableDict_);
            o.raw(102, "}");
        }
        o.raw(100, "AcDbSymbolTable");
    }
    o.i(70, static_cast<long long>(count));
    if (!r12_ && name == "DIMSTYLE") {
        o.raw(100, "AcDbDimStyleTable");
        o.i(71, static_cast<long long>(count));
    }
}

void Writer::tableEnd(Out& o) {
    o.zero("ENDTAB");
}

void Writer::tableRecord(Out& o, std::string_view type, std::uint64_t handle, std::uint64_t owner,
                         std::string_view subclass, std::string_view name, int flags) {
    o.zero(type);
    if (!r12_) {
        o.h(type == "DIMSTYLE" ? 105 : 5, handle);
        o.h(330, owner);
        o.raw(100, "AcDbSymbolTableRecord");
        o.raw(100, subclass);
    }
    o.str(2, name);
    if (flags >= 0)
        o.i(70, flags);
}

// A text or shape in a linetype is an element of its own, of length 0 (the reader folds it into
// the pattern at the offset where it sits). Shapes have no AutoCAD counterpart here: the name goes
// in group 9 and flag 4 tells the reader it is a shape.
void Writer::writeMark(Out& o, const LinetypeMark& m) {
    const bool shape = m.kind == LinetypeMark::Kind::Shape;
    using R = LinetypeMark::Rotation;
    o.d(49, 0);
    o.i(74, (shape ? 4 : 2) | (m.rotationKind == R::Absolute ? 1 : 0) |
                (m.rotationKind == R::Upright ? 8 : 0));
    // A shape named "#n" is shape number n of its shape file.
    int shapeNo = 0;
    if (shape && m.text.size() > 1 && m.text[0] == '#') {
        char* end = nullptr;
        const long n = std::strtol(m.text.c_str() + 1, &end, 10);
        if (end && *end == '\0' && n > 0 && n < 65536)
            shapeNo = static_cast<int>(n);
    }
    o.i(75, shapeNo);
    if (shape && !m.style.empty()) {
        auto& h = shapeFileHandle_[m.style];
        if (h == 0)
            h = newHandle();
        o.h(340, h);
    }
    if (!shape && !m.style.empty()) {
        for (const auto& [id, ts] : doc_.textStyles()) {
            if (ts.name != m.style)
                continue;
            auto& h = tsHandle_[id.value];
            if (h == 0)
                h = newHandle();
            o.h(340, h);
            break;
        }
    }
    o.d(46, m.scale);
    o.d(50, m.rotation * kDeg);
    o.d(44, m.shift.x);
    o.d(45, m.shift.y);
    if (!shapeNo)
        o.str(9, m.text);
}

void Writer::writeLinetypes(Out& o, std::uint64_t table) {
    std::vector<const Linetype*> rows;
    for (const auto& [id, lt] : doc_.linetypes()) {
        if (id != kLinetypeContinuous)
            rows.push_back(&lt);
    }
    tableBegin(o, "LTYPE", table, rows.size() + 3);
    struct Std {
        const char* name;
        const char* desc;
    };
    for (const Std& s : {Std{"ByBlock", ""}, Std{"ByLayer", ""}, Std{"Continuous", "Solid line"}}) {
        const std::uint64_t h = newHandle();
        ltHandle_[NameSet::fold(s.name)] = h;
        tableRecord(o, "LTYPE", h, table, "AcDbLinetypeTableRecord", s.name, 0);
        o.str(3, s.desc);
        o.i(72, 65);
        o.i(73, 0);
        o.d(40, 0);
    }
    for (const Linetype* lt : rows) {
        const std::uint64_t h = newHandle();
        ltHandle_[NameSet::fold(lt->name)] = h;
        tableRecord(o, "LTYPE", h, table, "AcDbLinetypeTableRecord", ltName_[lt->id.value], 0);
        o.str(3, lt->description);
        o.i(72, 65);
        double total = 0;
        for (double d : lt->pattern)
            total += std::abs(d);
        o.i(73, static_cast<long long>(lt->pattern.size() + (r12_ ? 0 : lt->marks.size())));
        o.d(40, total);
        std::size_t nextMark = 0;
        auto writeMarksUpTo = [&](double upTo) {
            while (!r12_ && nextMark < lt->marks.size() &&
                   lt->marks[nextMark].offset <= upTo + 1e-9)
                writeMark(o, lt->marks[nextMark++]);
        };
        double at = 0;
        writeMarksUpTo(0);
        for (double d : lt->pattern) {
            o.d(49, d);
            if (!r12_)
                o.i(74, 0);
            at += std::abs(d);
            writeMarksUpTo(at);
        }
        writeMarksUpTo(1e300);
    }
    tableEnd(o);
}

std::uint64_t Writer::pstyleHandle(const std::string& name) {
    auto same = [](const std::string& x, const std::string& y) {
        if (x.size() != y.size())
            return false;
        for (std::size_t i = 0; i < x.size(); ++i)
            if (std::tolower(static_cast<unsigned char>(x[i])) !=
                std::tolower(static_cast<unsigned char>(y[i])))
                return false;
        return true;
    };
    for (const auto& [known, handle] : pstyles_)
        if (same(known, name))
            return handle;
    pstyles_.emplace_back(name, newHandle());
    return pstyles_.back().second;
}

// The handle of a layer's table record. Sheets are written before the tables, so it is given out
// the first time anything asks for it.
std::uint64_t Writer::layerHandleOf(LayerId id) {
    auto [it, fresh] = layerHandle_.try_emplace(id.value, 0);
    if (fresh)
        it->second = newHandle();
    return it->second;
}

void Writer::writeLayers(Out& o, std::uint64_t table) {
    layerTable_ = table;
    if (!r12_) {
        const auto it = doc_.header().custom.find(kKeptObjectRootKey);
        if (it != doc_.header().custom.end() &&
            it->second.find("%LAYERTABLE%") != std::string::npos &&
            doc_.header().custom.count(kKeptObjectsKey))
            layerTableDict_ = newHandle();
    }
    tableBegin(o, "LAYER", table, doc_.layers().size());
    std::set<std::uint32_t> newVpFrozen;
    if (const auto it = doc_.header().custom.find("tbcad.vpnewfrozen");
        it != doc_.header().custom.end())
        for (const std::string& part : splitOn(it->second, ','))
            newVpFrozen.insert(static_cast<std::uint32_t>(std::strtoul(part.c_str(), nullptr, 10)));
    std::size_t flattened = 0;
    for (const auto& [id, layer] : doc_.layers()) {
        EffectiveLayer eff = doc_.effective(id);
        // Active Only locks the other layers for this session only; the file keeps their own lock.
        if (eff.inactive)
            eff.locked = false;
        // Flag 2: frozen in new viewports.
        int flags =
            (eff.frozen ? 1 : 0) | (eff.locked ? 4 : 0) | (newVpFrozen.count(id.value) ? 2 : 0);
        const std::uint64_t layerRec = layerHandleOf(id);
        tableRecord(o, "LAYER", layerRec, table, "AcDbLayerTableRecord", layerName_[id.value],
                    flags);
        // Sub-layers carry their effective look so that other programs show the same drawing;
        // the TBCAD XDATA below restores the tree and the ByParent settings.
        AciChoice c = aciFor(eff.color);
        o.i(62, eff.on ? c.aci : -c.aci);
        if (c.rgb && !r12_)
            o.i(420, *c.rgb);
        auto lt = ltName_.find(eff.linetype.value);
        o.str(6, lt == ltName_.end() ? "CONTINUOUS" : lt->second);
        if (!r12_) {
            o.i(290, eff.plot ? 1 : 0);
            o.i(370, eff.lineweight.isValue() ? validLineweight(eff.lineweight.v) : -3);
            if (layer.transparency)
                o.i(440, transparencyGroup(layer.transparency));
            // A named plot style; "Normal" is the default and needs no pointer.
            const std::string style = layerPlotStyle(doc_.header(), id);
            if (!style.empty() && style != "Normal")
                o.h(390, pstyleHandle(style));
        }
        if (!layer.description.empty() && !r12_) {
            o.raw(1001, "AcAecLayerStandard");
            o.str(1000, "");
            o.str(1000, layer.description);
        }
        if (eff.on != layer.on || eff.frozen != layer.frozen || eff.locked != layer.locked ||
            eff.plot != layer.plot) {
            ++flattened;
        }
        if (layer.parent) {
            auto parent = layerName_.find(layer.parent.value);
            if (parent != layerName_.end()) {
                int xflags = (layer.color.kind == ColorKind::ByParent ? 1 : 0) |
                             (layer.linetype == kLinetypeByParent ? 2 : 0) |
                             (layer.lineweight.isByParent() ? 4 : 0);
                o.raw(1001, "TBCAD");
                o.str(1000, "parent=" + parent->second);
                o.str(1000, "name=" + layer.name);
                o.i(1070, xflags);
            }
        }
    }
    if (flattened > 0) {
        rep_.info(wcode::kLayerFlattened,
                  std::to_string(flattened) +
                      " sub-layers inherit on/frozen/locked/plot from a parent; DXF stores the "
                      "effective state, so the parent's state is copied into each of them");
    }
    tableEnd(o);
}

namespace {
bool isStrokeFont(const TextStyle& ts) {
    auto lower = [](std::string v) {
        for (char& c : v)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return v;
    };
    const std::string file = lower(ts.fontFile);
    if (file.size() >= 5 && file.compare(file.size() - 5, 5, ".tbsf") == 0)
        return true;
    return ts.fontFile.empty() && lower(ts.fontFamily) == "tbcad stroke";
}
}  // namespace

double styleWidthWritten(const TextStyle& ts) {
    return isStrokeFont(ts) ? std::round(ts.widthFactor * 0.92 * 1e9) / 1e9 : ts.widthFactor;
}

void Writer::writeStyles(Out& o, std::uint64_t table) {
    tableBegin(o, "STYLE", table, doc_.textStyles().size() + shapeFileHandle_.size());
    for (const auto& [id, ts] : doc_.textStyles()) {
        std::uint64_t& reserved = tsHandle_[id.value];
        if (reserved == 0)
            reserved = newHandle();
        tableRecord(o, "STYLE", reserved, table, "AcDbTextStyleTableRecord", tsName_[id.value], 0);
        o.d(40, ts.height);
        // The reader widens the stroke font that stands in for txt.shx by 1/0.92; undo that here.
        o.d(41, styleWidthWritten(ts));
        o.d(50, ts.oblique * kDeg);
        o.i(71, (ts.backward ? 2 : 0) | (ts.upsideDown ? 4 : 0));
        o.d(42, 2.5);
        std::string font = ts.fontFile;
        if (font.empty())
            font = ts.fontFamily.empty() ? "txt" : ts.fontFamily + ".ttf";
        // TBCAD's own stroke font has no AutoCAD counterpart; txt.shx is AutoCAD's stand-in and
        // reads back as the TBCAD Stroke font.
        if (isStrokeFont(ts))
            font = ts.shxFile.empty() ? "txt.shx" : ts.shxFile;
        o.str(3, font);
        o.str(4, ts.bigFontFile);
        if (ts.annotative && !r12_) {
            o.str(1001, "AcadAnnotative");
            o.str(1000, "AnnotativeData");
            o.str(1002, "{");
            o.i(1070, 1);
            o.i(1070, 0);
            o.str(1002, "}");
        }
    }
    // Shape files that linetypes draw from: a style record of the file, flag 1.
    for (const auto& [file, handle] : shapeFileHandle_) {
        std::string stem = file.substr(0, file.rfind('.'));
        tableRecord(o, "STYLE", handle, table, "AcDbTextStyleTableRecord", stem, 1);
        o.d(40, 0);
        o.d(41, 1);
        o.d(50, 0);
        o.i(71, 0);
        o.d(42, 2.5);
        o.str(3, file);
        o.str(4, "");
    }
    tableEnd(o);
}

void Writer::writeDimStyles(Out& o, std::uint64_t table) {
    tableBegin(o, "DIMSTYLE", table, doc_.dimStyles().size());
    auto clr = [](Color c) {
        if (c.kind == ColorKind::ByBlock)
            return 0;
        if (c.kind == ColorKind::ByLayer)
            return 256;
        return aciFor(c).aci;
    };
    for (const auto& [id, ds] : doc_.dimStyles()) {
        tableRecord(o, "DIMSTYLE", newHandle(), table, "AcDbDimStyleTableRecord", dsName_[id.value],
                    0);
        if (!ds.prefix.empty() || !ds.suffix.empty())
            o.str(3, ds.prefix + "<>" + ds.suffix);
        // DIMTXSTY: the text style's STYLE record (the STYLE table is written first).
        if (const auto t = tsHandle_.find(ds.textStyle.value);
            !r12_ && ds.textStyle.value && t != tsHandle_.end() && t->second != 0)
            o.h(340, t->second);
        o.d(40, ds.overallScale);
        o.d(41, ds.arrowSize);
        o.d(42, ds.extLineOffset);
        o.d(44, ds.extLineExtension);
        if (ds.roundOff > 0)
            o.d(45, ds.roundOff);
        o.d(46, ds.dimLineExtension);
        if (ds.baselineSpacing > 0)
            o.d(43, ds.baselineSpacing);  // DIMDLI
        o.d(141, ds.centerMark);          // DIMCEN
        if (ds.tolerance != DimTolerance::None) {
            o.i(ds.tolerance == DimTolerance::Limits ? 72 : 71, 1);
            o.d(47, ds.tolUpper);
            o.d(48, ds.tolerance == DimTolerance::Symmetrical ? ds.tolUpper : ds.tolLower);
        }
        if (ds.altUnits) {
            o.i(170, 1);
            o.d(143, ds.altScale / mmPerUnit(ds.altUnit));
            o.i(171, std::clamp(ds.altPrecision, 0, 16));
            if (!ds.altPrefix.empty() || !ds.altSuffix.empty())
                o.str(4, ds.altPrefix + "[]" + ds.altSuffix);
        }
        if (ds.linearFactor != 1)
            o.d(144, ds.linearFactor);  // DIMLFAC
        if (ds.textVertPos != 0)
            o.d(145, ds.textVertPos);  // DIMTVP
        if (ds.tolHeightFactor != 1)
            o.d(146, ds.tolHeightFactor);                                  // DIMTFAC
        o.i(73, ds.textAlign == DimTextAlign::Horizontal ? 1 : 0);         // DIMTIH
        o.i(74, ds.textAlignOutside == DimTextAlign::Horizontal ? 1 : 0);  // DIMTOH
        o.i(75, ds.suppressExt1 ? 1 : 0);                                  // DIMSE1
        o.i(76, ds.suppressExt2 ? 1 : 0);                                  // DIMSE2
        o.i(172, ds.forceLineInside ? 1 : 0);                              // DIMTOFL
        o.i(174, ds.textInsideForced ? 1 : 0);                             // DIMTIX
        o.i(175, ds.suppressArrowsOutside ? 1 : 0);                        // DIMSOXD
        o.i(77, ds.textPlacement == DimTextPlacement::AboveLine ? 1
                : ds.textPlacement == DimTextPlacement::Below   ? 4
                                                                : 0);
        o.i(78, (ds.suppressLeadingZeros ? 4 : 0) | (ds.suppressTrailingZeros ? 8 : 0));
        {
            // DIMAZIN, DIMTZIN, DIMALTZ (a style that follows the length setting writes it).
            auto bits = [&](int z) {
                return z >= 0
                           ? z
                           : (ds.suppressLeadingZeros ? 1 : 0) | (ds.suppressTrailingZeros ? 2 : 0);
            };
            auto zin = [&](int z) { return ((bits(z) & 1) ? 4 : 0) | ((bits(z) & 2) ? 8 : 0); };
            if (!r12_) {
                o.i(79, bits(ds.angularZeros));
                o.i(284, zin(ds.tolZeros));
                o.i(285, zin(ds.altZeros));
            }
        }
        o.d(140, ds.textHeight);
        o.d(147, ds.textFrame ? -ds.textGap : ds.textGap);  // DIMGAP (negative: framed)
        o.i(69, ds.textFill);                               // DIMTFILL
        if (ds.textFillColor > 0)
            o.i(70, ds.textFillColor);  // DIMTFILLCLR
        o.i(176, clr(ds.dimLineColor));
        o.i(177, clr(ds.extLineColor));
        o.i(178, clr(ds.textColor));
        if (!r12_) {
            o.i(371, ds.dimLineWeight.v);  // DIMLWD
            o.i(372, ds.extLineWeight.v);  // DIMLWE
            // DIMLTYPE, DIMLTEX1, DIMLTEX2: the LTYPE records (the LTYPE table is written first).
            for (const auto& [code, name] :
                 {std::pair<int, const std::string*>{345, &ds.dimLinetype},
                  {346, &ds.ext1Linetype},
                  {347, &ds.ext2Linetype}})
                if (const auto l = ltHandle_.find(NameSet::fold(*name));
                    !name->empty() && l != ltHandle_.end())
                    o.h(code, l->second);
            o.i(271, std::clamp(ds.precision, 0, 16));
            o.i(280, ds.textJustify);                    // DIMJUST
            o.i(275, static_cast<int>(ds.angleFormat));  // DIMAUNIT (same numbering)
            o.i(277, ds.linearFormat == DimLinearFormat::Architectural ? 4
                     : ds.linearFormat == DimLinearFormat::Fractional  ? 5
                                                                       : 2);  // DIMLUNIT
            if (ds.angularPrecision >= 0)
                o.i(179, ds.angularPrecision);      // DIMADEC (absent: the same as DIMDEC)
            o.i(278, ds.commaDecimal ? ',' : '.');  // DIMDSEP
            o.i(281, ds.suppressDim1 ? 1 : 0);      // DIMSD1
            o.i(282, ds.suppressDim2 ? 1 : 0);      // DIMSD2
            // DIMTMOVE (279) and DIMATFIT (289); 0 and 3 are the AutoCAD defaults.
            o.i(279, static_cast<int>(ds.textMove));
            o.i(289, ds.fit == DimFit::BothOutside   ? 0
                     : ds.fit == DimFit::ArrowsFirst ? 1
                     : ds.fit == DimFit::TextFirst   ? 2
                                                     : 3);
            if (ds.tolerance != DimTolerance::None)
                o.i(272, std::clamp(ds.tolPrecision, 0, 16));
            // Fixed length extension lines go as extended data, as AutoCAD writes them.
            if (ds.hasFixedExt()) {
                o.raw(1001, "ACAD_DSTYLE_DIMEXT_ENABLED");
                o.i(1070, 383);
                o.i(1070, ds.fixedExtLines ? 1 : 0);
                o.raw(1001, "ACAD_DSTYLE_DIMEXT_LENGTH");
                o.i(1070, 378);
                o.d(1040, ds.fixedExtLength);
            }
            // The arrow kind has no group TBCAD can write (DIMBLK wants a block record's handle):
            // it travels as extended data of the style.
            if (ds.arrowType != ArrowType::ClosedFilled ||
                ds.leaderArrow != ArrowType::ClosedFilled) {
                o.raw(1001, "TBCAD");
                if (ds.arrowType != ArrowType::ClosedFilled)
                    o.raw(1000, "arrow=" + std::to_string(static_cast<int>(ds.arrowType)));
                if (ds.leaderArrow != ArrowType::ClosedFilled)
                    o.raw(1000, "leaderArrow=" + std::to_string(static_cast<int>(ds.leaderArrow)));
            }
        }
    }
    tableEnd(o);
}

void Writer::writeBlockRecords(Out& o, std::uint64_t table) {
    tableBegin(
        o, "BLOCK_RECORD", table,
        userBlocks_.size() + dimBlocks_.size() + 2 + (sheets_.size() > 1 ? sheets_.size() - 1 : 0));
    tableRecord(o, "BLOCK_RECORD", modelRec_, table, "AcDbBlockTableRecord", "*Model_Space", -1);
    o.h(340, layoutModel_);
    tableRecord(o, "BLOCK_RECORD", paperRec_, table, "AcDbBlockTableRecord", "*Paper_Space", -1);
    o.h(340, layoutPaper_);
    for (std::size_t i = 1; i < sheets_.size(); ++i) {
        tableRecord(o, "BLOCK_RECORD", sheets_[i].rec, table, "AcDbBlockTableRecord",
                    "*Paper_Space" + std::to_string(i - 1), -1);
        o.h(340, sheets_[i].layout);
    }
    for (BlockId id : userBlocks_) {
        tableRecord(o, "BLOCK_RECORD", blockRec_[id.value], table, "AcDbBlockTableRecord",
                    blockName_[id.value], -1);
        o.i(340, 0);
    }
    for (const DimBlock& b : dimBlocks_) {
        tableRecord(o, "BLOCK_RECORD", b.rec, table, "AcDbBlockTableRecord", b.name, -1);
        o.i(340, 0);
    }
    tableEnd(o);
}

void Writer::blockBegin(Out& o, const std::string& name, std::uint64_t rec, Vec2 base, int flags,
                        bool paperSpace, const BlockDef* xref) {
    o.zero("BLOCK");
    if (!r12_) {
        o.h(5, newHandle());
        o.h(330, rec);
        o.raw(100, "AcDbEntity");
        if (paperSpace)
            o.i(67, 1);
    }
    o.raw(8, "0");
    if (!r12_)
        o.raw(100, "AcDbBlockBegin");
    o.str(2, name);
    o.i(70, flags);
    o.pt(10, base);
    o.str(3, name);
    o.raw(1, "");
    if (xref && !xref->xrefPath.empty()) {
        // The block is written with its content (the snapshot), as an ordinary block; TBCAD
        // restores the external reference from this XDATA.
        o.raw(1001, "TBCAD");
        o.str(1000, "xref=" + xref->xrefPath);
        o.str(1000, "stamp=" + std::to_string(xref->xrefStamp));
        o.i(1070, xref->xrefOverlay ? 1 : 0);
    }
}

void Writer::blockEnd(Out& o, std::uint64_t rec, bool paperSpace) {
    o.zero("ENDBLK");
    if (!r12_) {
        o.h(5, newHandle());
        o.h(330, rec);
        o.raw(100, "AcDbEntity");
        if (paperSpace)
            o.i(67, 1);
    }
    o.raw(8, "0");
    if (!r12_)
        o.raw(100, "AcDbBlockEnd");
}

std::string Writer::tablesSection() {
    Out o(utf8_, r12_);
    o.zero("SECTION");
    o.raw(2, "TABLES");

    // VPORT: one active viewport showing the drawing extents.
    {
        std::uint64_t table = newHandle();
        tableBegin(o, "VPORT", table, 1);
        tableRecord(o, "VPORT", newHandle(), table, "AcDbViewportTableRecord", "*Active", 0);
        BBox2 b = extents_;
        Vec2 center = b.empty() ? Vec2{0, 0} : b.center();
        double height = b.empty() ? 1000.0 : std::max(b.height(), b.width() / 1.5) * 1.1;
        if (!(height > 0))
            height = 1000.0;
        o.pt2(10, {0, 0});
        o.pt2(11, {1, 1});
        o.pt2(12, center);
        o.pt2(13, {0, 0});
        o.pt2(14, {10, 10});
        o.pt2(15, {10, 10});
        o.d(16, 0);
        o.d(26, 0);
        o.d(36, 1);
        o.d(17, 0);
        o.d(27, 0);
        o.d(37, 0);
        o.d(40, height);
        o.d(41, 1.5);
        o.d(42, 50);
        o.d(43, 0);
        o.d(44, 0);
        o.d(50, 0);
        o.d(51, 0);
        o.i(71, 0);
        o.i(72, 100);
        o.i(73, 1);
        o.i(74, 3);
        o.i(75, 0);
        o.i(76, 0);
        o.i(77, 0);
        o.i(78, 0);
        tableEnd(o);
    }
    writeLinetypes(o, newHandle());
    writeLayers(o, newHandle());
    writeStyles(o, newHandle());
    {
        std::uint64_t table = newHandle();
        tableBegin(o, "VIEW", table, 0);
        tableEnd(o);
        table = newHandle();
        // Named coordinate systems (Header::custom["tbcad.ucs"], name TAB x TAB y TAB angle): the
        // origin and the X and Y axis directions, as in a UCS table record.
        struct Row {
            std::string name;
            double x, y, a;
        };
        std::vector<Row> ucs;
        if (auto it = doc_.header().custom.find("tbcad.ucs"); it != doc_.header().custom.end()) {
            std::istringstream in(it->second);
            std::string line;
            while (std::getline(in, line)) {
                std::vector<std::string> f;
                std::size_t from = 0;
                for (std::size_t t; (t = line.find('\t', from)) != std::string::npos; from = t + 1)
                    f.push_back(line.substr(from, t - from));
                f.push_back(line.substr(from));
                if (f.size() == 4 && !f[0].empty())
                    ucs.push_back({f[0], std::strtod(f[1].c_str(), nullptr),
                                   std::strtod(f[2].c_str(), nullptr),
                                   std::strtod(f[3].c_str(), nullptr)});
            }
        }
        tableBegin(o, "UCS", table, ucs.size());
        for (const Row& u : ucs) {
            tableRecord(o, "UCS", newHandle(), table, "AcDbUCSTableRecord", u.name, 0);
            o.pt(10, {u.x, u.y});
            o.d(30, 0);
            o.pt(11, {std::cos(u.a), std::sin(u.a)});
            o.d(31, 0);
            o.pt(12, {-std::sin(u.a), std::cos(u.a)});
            o.d(32, 0);
        }
        tableEnd(o);
    }
    {
        std::uint64_t table = newHandle();
        // Every application that has XDATA on an entity needs a record of its own.
        std::set<std::string> extApps;
        doc_.forEachEntity([&](const Entity& e) {
            for (const XdataApp& app : e.common.ext.get().xdata)
                if (!app.app.empty() && app.app != "ACAD" && app.app != "TBCAD")
                    extApps.insert(app.app);
            if (const auto* ins = entityAs<InsertData>(e); ins && ins->array)
                extApps.insert("TBCAD_ARRAY");
            if (kind(e) == EntityKind::MLeader)
                extApps.insert("TBCAD_MLEADER");
            if (const auto* d = entityAs<DimensionData>(e)) {
                bool assoc = false;
                for (const DimAssoc& a : d->assoc)
                    assoc = assoc || a.kind != DimAssoc::Kind::None;
                if (assoc || !d->breaks.empty() || !d->jogs.empty() || d->flipArrow1 ||
                    d->flipArrow2)
                    extApps.insert("TBCAD_DIM");
            } else if (const auto* l = entityAs<LeaderData>(e);
                       l && (!l->breaks.empty() || leaderKeepsTextPos(*l))) {
                extApps.insert("TBCAD_DIM");
            } else if (const auto* h = entityAs<HatchData>(e); h && hatchNamesSources(*h)) {
                extApps.insert("TBCAD_DIM");
            }
        });
        bool layerNotes = false;
        for (const auto& [lid, layer] : doc_.layers())
            layerNotes = layerNotes || !layer.description.empty();
        if (layerNotes && !r12_)
            extApps.insert("AcAecLayerStandard");
        for (const auto& [id, ds] : doc_.dimStyles())
            if (ds.hasFixedExt() && !r12_) {
                extApps.insert("ACAD_DSTYLE_DIMEXT_ENABLED");
                extApps.insert("ACAD_DSTYLE_DIMEXT_LENGTH");
            }
        tableBegin(o, "APPID", table, 2 + extApps.size());
        tableRecord(o, "APPID", newHandle(), table, "AcDbRegAppTableRecord", "ACAD", 0);
        tableRecord(o, "APPID", newHandle(), table, "AcDbRegAppTableRecord", "TBCAD", 0);
        for (const std::string& app : extApps)
            tableRecord(o, "APPID", newHandle(), table, "AcDbRegAppTableRecord", app, 0);
        levelsXdata(o);
        tableEnd(o);
    }
    writeDimStyles(o, newHandle());
    if (!r12_)
        writeBlockRecords(o, newHandle());
    o.zero("ENDSEC");
    return std::move(o.buf);
}

// The level table travels as XDATA on the TBCAD application record: the current level, how
// other levels show, and one "level=<name>" row (elevation, flags) per level so that empty
// levels and their visible / locked state survive. Entities name their level the same way.
void Writer::levelsXdata(Out& o) {
    if (!tagLevels_)
        return;
    o.raw(1001, "TBCAD");
    const Level* cur = doc_.level(doc_.header().currentLevel);
    o.str(1000, "levels-current=" + (cur ? cur->name : std::string()));
    o.str(1000, "levels-view=" + std::to_string(static_cast<int>(doc_.header().levelView)));
    for (const auto& [id, l] : doc_.levels()) {
        o.str(1000, "levels-row=" + l.name);
        o.d(1040, l.elevation);
        o.i(1070, (l.visible ? 1 : 0) | (l.locked ? 2 : 0));
    }
}

std::string Writer::objectsSection() {
    Out o(utf8_, r12_);
    o.zero("SECTION");
    o.raw(2, "OBJECTS");

    std::uint64_t groupDict = newHandle(), layoutDict = newHandle(), mlineDict = newHandle(),
                  plotDict = newHandle(), mlineStyle = newHandle(), placeholder = newHandle();

    struct GroupOut {
        std::string name, description;
        std::uint64_t handle = 0;
        bool selectable = true;
        std::vector<std::uint64_t> members;
    };
    std::vector<GroupOut> groupOut;
    std::map<std::string, std::string> groupDesc;
    if (auto dit = doc_.header().custom.find(kKeptGroupDescKey);
        dit != doc_.header().custom.end()) {
        std::string_view text = dit->second;
        while (!text.empty()) {
            const std::size_t nl = text.find('\n');
            const std::string_view line = text.substr(0, nl);
            text = nl == std::string_view::npos ? std::string_view{} : text.substr(nl + 1);
            if (const std::size_t tab = line.find('\t'); tab != std::string_view::npos)
                groupDesc[std::string(line.substr(0, tab))] = std::string(line.substr(tab + 1));
        }
    }
    if (!r12_) {
        for (const tbcad::Group& g : readGroups(doc_)) {
            GroupOut out;
            out.name = g.name;
            if (auto d = groupDesc.find(g.name); d != groupDesc.end())
                out.description = d->second;
            out.selectable = g.selectable;
            for (EntityId m : g.members) {
                auto it = groupHandle_.find(m.value);
                if (it != groupHandle_.end() && it->second)
                    out.members.push_back(it->second);
            }
            if (out.members.empty())
                continue;
            out.handle = newHandle();
            groupOut.push_back(std::move(out));
        }
    }

    // Objects kept from the source file: a new handle for each "@N" token.
    const auto& keptCustom = doc_.header().custom;
    std::unordered_map<std::string, std::uint64_t> tokenHandle;
    auto handleOfToken = [&](const std::string& t) -> std::uint64_t {
        if (t == "@root")
            return rootDict_;
        if (!t.empty() && t[0] == '#') {  // an entity of the source file, if it was written
            const std::uint64_t h = std::strtoull(t.c_str() + 1, nullptr, 16);
            return L().handlesWritten.count(h) ? h : 0;
        }
        if (t.rfind("$S:", 0) == 0) {  // a text style of the drawing, by name
            for (const auto& [id, ts] : doc_.textStyles()) {
                if (tsName_[id.value] != t.substr(3))
                    continue;
                std::uint64_t& h = tsHandle_[id.value];
                if (h == 0)
                    h = newHandle();
                return h;
            }
            return 0;
        }
        if (t.size() < 2 || t[0] != '@')
            return 0;
        auto [it, fresh] = tokenHandle.emplace(t, 0);
        if (fresh)
            it->second = newHandle();
        return it->second;
    };
    KeptGroups keptObjects, keptRootEntries;
    if (auto it = keptCustom.find(kKeptObjectsKey); it != keptCustom.end())
        keptObjects = unpackGroups(it->second);
    if (auto it = keptCustom.find(kKeptObjectRootKey); it != keptCustom.end()) {
        // lines "name TAB @N" (the name is a group value, the token its handle)
        std::string_view text = it->second;
        while (!text.empty()) {
            const std::size_t nl = text.find('\n');
            const std::string_view line = text.substr(0, nl);
            text = nl == std::string_view::npos ? std::string_view{} : text.substr(nl + 1);
            const std::size_t tab = line.find('\t');
            if (tab != std::string_view::npos)
                keptRootEntries.emplace_back(
                    0, std::string(line.substr(0, tab)) + '\t' + std::string(line.substr(tab + 1)));
        }
    }
    if (keptObjects.empty())
        keptRootEntries.clear();

    // Annotation scales: the current scale and the list, when the drawing has any (R2007 and later
    // get real SCALE objects; the XRECORD carries them for every version, for our own importer).
    const auto& custom = doc_.header().custom;
    const bool hasScales =
        !r12_ && (custom.count(kAnnoScaleKey) || custom.count(kAnnoScaleListKey));
    const bool scaleObjects = hasScales && opt_.version == DxfVersion::R2007;
    const std::uint64_t imageDict = images_.empty() ? 0 : newHandle();
    const std::uint64_t imageVars = images_.empty() ? 0 : newHandle();
    const std::uint64_t pdfDict = pdfs_.empty() ? 0 : newHandle();
    const bool hasWipeoutVars =
        !r12_ && (wipeouts_ || doc_.header().custom.count(kWipeoutFrameKey) != 0);
    const std::uint64_t wipeoutVars = hasWipeoutVars ? newHandle() : 0;
    std::uint64_t scaleDict = scaleObjects ? newHandle() : 0;
    std::uint64_t scaleRec = hasScales ? newHandle() : 0;
    constexpr const char* kSketchConstraints = "tbcad.constraints";
    constexpr const char* kSketchParameters = "tbcad.parameters";
    // Constraints and parameters (docs/constraints.md): our own XRECORDs, one line per code-1
    // group.
    static const char* const kSketchKeys[2][2] = {{kSketchConstraints, "TBCAD_CONSTRAINTS"},
                                                  {kSketchParameters, "TBCAD_PARAMETERS"}};
    std::uint64_t sketchRec[2] = {0, 0};
    for (int k = 0; k < 2 && !r12_; ++k) {
        auto it = custom.find(kSketchKeys[k][0]);
        if (it != custom.end() && !it->second.empty())
            sketchRec[k] = newHandle();
    }
    // Every other TBCAD setting kept in the header (multileader and table styles, layer states and
    // filters, view names, ...): one XRECORD of "K<key>" / "V<escaped value chunk>" lines.
    // (Only families no other part of the file carries: header variables, groups, layouts,
    // constraints, UCS and the like have their own places.)
    static const char* const kSettingFamilies[] = {
        "tbcad.mleaderstyle.",  "tbcad.mleadercurrent", "tbcad.layerstate.", "tbcad.tablestyle.",
        "tbcad.layiso",         "tbcad.layerfilter.",   "tbcad.layergroup.", "tbcad.layerp.",
        "tbcad.view.",          "tbcad.prop.",          "tbcad.textlayer",   "tbcad.hatchlayer",
        "tbcad.dimlayer",       "tbcad.insbase",        "tbcad.link.",       "tbcad.lintypes",
        "tbcad.selannodisplay", "tbcad.spell.words",    "tbcad.annonew",     "tbcad.pdfosnap"};
    std::vector<std::pair<std::string, std::string>> settings;
    if (!r12_)
        for (const auto& [key, value] : custom) {
            if (value.empty() || value.size() >= (8u << 20))
                continue;
            for (const char* family : kSettingFamilies)
                if (key.rfind(family, 0) == 0) {
                    settings.emplace_back(key, value);
                    break;
                }
        }
    const std::uint64_t settingsRec = settings.empty() ? 0 : newHandle();
    std::vector<double> scaleList = annotationScaleList(doc_.header());
    std::vector<std::uint64_t> scaleHandles;
    for (std::size_t i = 0; scaleObjects && i < scaleList.size(); ++i)
        scaleHandles.push_back(newHandle());

    o.zero("DICTIONARY");
    o.h(5, rootDict_);
    o.i(330, 0);
    o.raw(100, "AcDbDictionary");
    o.i(281, 1);
    if (scaleObjects) {
        o.raw(3, "ACAD_SCALELIST");
        o.h(350, scaleDict);
    }
    if (hasScales) {
        o.raw(3, "TBCAD_ANNOSCALE");
        o.h(350, scaleRec);
    }
    if (settingsRec) {
        o.raw(3, "TBCAD_SETTINGS");
        o.h(350, settingsRec);
    }
    for (int k = 0; k < 2; ++k) {
        if (!sketchRec[k])
            continue;
        o.raw(3, kSketchKeys[k][1]);
        o.h(350, sketchRec[k]);
    }
    o.raw(3, "ACAD_GROUP");
    o.h(350, groupDict);
    if (hasWipeoutVars) {
        o.raw(3, "ACAD_WIPEOUT_VARS");
        o.h(350, wipeoutVars);
    }
    if (pdfDict) {
        o.raw(3, "ACAD_PDFDEFINITIONS");
        o.h(350, pdfDict);
    }
    if (!images_.empty()) {
        o.raw(3, "ACAD_IMAGE_DICT");
        o.h(350, imageDict);
        o.raw(3, "ACAD_IMAGE_VARS");
        o.h(350, imageVars);
    }
    o.raw(3, "ACAD_LAYOUT");
    o.h(350, layoutDict);
    o.raw(3, "ACAD_MLINESTYLE");
    o.h(350, mlineDict);
    o.raw(3, "ACAD_PLOTSTYLENAME");
    o.h(350, plotDict);
    for (const auto& [unused, entry] : keptRootEntries) {
        if (entry.rfind("%LAYERTABLE%", 0) == 0)
            continue;
        const std::size_t tab = entry.find('\t');
        o.str(3, entry.substr(0, tab));
        o.h(350, handleOfToken(entry.substr(tab + 1)));
    }

    auto ownedDict = [&](std::uint64_t handle) {
        o.zero("DICTIONARY");
        o.h(5, handle);
        o.raw(102, "{ACAD_REACTORS");
        o.h(330, rootDict_);
        o.raw(102, "}");
        o.h(330, rootDict_);
        o.raw(100, "AcDbDictionary");
        o.i(281, 1);
    };
    if (layerTableDict_) {  // the layer filters: the LAYER table's extension dictionary
        o.zero("DICTIONARY");
        o.h(5, layerTableDict_);
        o.raw(102, "{ACAD_REACTORS");
        o.h(330, layerTable_);
        o.raw(102, "}");
        o.h(330, layerTable_);
        o.raw(100, "AcDbDictionary");
        o.i(281, 1);
        for (const auto& [unused, entry] : keptRootEntries) {
            if (entry.rfind("%LAYERTABLE%", 0) != 0)
                continue;
            const std::size_t tab = entry.find('\t');
            o.str(3, entry.substr(12, tab - 12));
            o.h(350, handleOfToken(entry.substr(tab + 1)));
        }
    }
    ownedDict(groupDict);
    for (const GroupOut& g : groupOut) {
        o.str(3, g.name);
        o.h(350, g.handle);
    }
    if (scaleObjects) {
        ownedDict(scaleDict);
        for (std::size_t i = 0; i < scaleList.size(); ++i) {
            char name[48];
            std::snprintf(name, sizeof name, "1:%g", scaleList[i]);
            o.raw(3, name);
            o.h(350, scaleHandles[i]);
        }
        for (std::size_t i = 0; i < scaleList.size(); ++i) {
            char name[48];
            std::snprintf(name, sizeof name, "1:%g", scaleList[i]);
            o.zero("SCALE");
            o.h(5, scaleHandles[i]);
            o.raw(102, "{ACAD_REACTORS");
            o.h(330, scaleDict);
            o.raw(102, "}");
            o.h(330, scaleDict);
            o.raw(100, "AcDbScale");
            o.i(70, 0);
            o.raw(300, name);
            o.d(140, 1.0);
            o.d(141, scaleList[i]);
            o.i(290, 0);
        }
    }
    if (hasScales) {
        // current scale and list as text: "current=50;list=1,2,5"
        char cur[48];
        std::snprintf(cur, sizeof cur, "%.12g", currentAnnotationScale(doc_.header()));
        std::string text = std::string("current=") + cur + ";list=";
        for (std::size_t i = 0; i < scaleList.size(); ++i) {
            char v[48];
            std::snprintf(v, sizeof v, "%.12g", scaleList[i]);
            text += (i ? "," : "") + std::string(v);
        }
        o.zero("XRECORD");
        o.h(5, scaleRec);
        o.h(330, rootDict_);
        o.raw(100, "AcDbXrecord");
        o.i(280, 1);
        o.str(1, text);
    }
    for (int k = 0; k < 2; ++k) {
        if (!sketchRec[k])
            continue;
        o.zero("XRECORD");
        o.h(5, sketchRec[k]);
        o.h(330, rootDict_);
        o.raw(100, "AcDbXrecord");
        o.i(280, 1);
        o.str(1, kSketchKeys[k][0]);
        const std::string& body = custom.at(kSketchKeys[k][0]);
        std::size_t at = 0;
        while (at < body.size()) {
            std::size_t nl = body.find('\n', at);
            if (nl == std::string::npos)
                nl = body.size();
            if (nl > at)
                o.str(1, std::string_view(body).substr(at, nl - at));
            at = nl + 1;
        }
    }

    if (settingsRec) {
        o.zero("XRECORD");
        o.h(5, settingsRec);
        o.h(330, rootDict_);
        o.raw(100, "AcDbXrecord");
        o.i(280, 1);
        o.str(1, "tbcad.settings");
        for (const auto& [key, value] : settings) {
            o.str(1, "K" + key);
            std::string esc;
            for (char c : value) {
                if (c == '\\')
                    esc += "\\\\";
                else if (c == '\n')
                    esc += "\\n";
                else if (c == '\r')
                    esc += "\\r";
                else
                    esc.push_back(c);
            }
            // Cut at UTF-8 boundaries: a group holds about 2000 bytes.
            for (std::size_t at = 0; at < esc.size();) {
                std::size_t n = std::min<std::size_t>(1000, esc.size() - at);
                while (at + n < esc.size() &&
                       (static_cast<unsigned char>(esc[at + n]) & 0xC0) == 0x80)
                    --n;
                o.str(1, "V" + esc.substr(at, n));
                at += n;
            }
        }
    }

    if (hasWipeoutVars) {
        o.zero("WIPEOUTVARIABLES");
        o.h(5, wipeoutVars);
        o.h(330, rootDict_);
        o.raw(100, "AcDbWipeoutVariables");
        o.i(70, wipeoutFrameMode(doc_.header()));
    }

    if (!images_.empty()) {
        // IMAGEFRAME: the frame is shown unless every picture has it hidden.
        bool anyFrame = false;
        for (const ImageOut& im : images_)
            anyFrame = anyFrame || im.frame;
        o.zero("RASTERVARIABLES");
        o.h(5, imageVars);
        o.raw(102, "{ACAD_REACTORS");
        o.h(330, rootDict_);
        o.raw(102, "}");
        o.h(330, rootDict_);
        o.raw(100, "AcDbRasterVariables");
        o.i(90, 0);
        o.i(70, anyFrame ? 1 : 0);
        o.i(71, 1);
        o.i(72, 0);
        ownedDict(imageDict);
        std::set<std::string> used;
        std::vector<std::string> keys;
        for (const ImageOut& im : images_) {
            std::string key = cleanName(im.name, "image");
            for (int n = 2; !used.insert(key).second; ++n)
                key = cleanName(im.name, "image") + "_" + std::to_string(n);
            keys.push_back(key);
            o.str(3, key);
            o.h(350, im.def);
        }
        for (const ImageOut& im : images_) {
            o.zero("IMAGEDEF");
            o.h(5, im.def);
            o.raw(102, "{ACAD_REACTORS");
            o.h(330, imageDict);
            o.h(330, im.reactor);
            o.raw(102, "}");
            o.h(330, imageDict);
            o.raw(100, "AcDbRasterImageDef");
            o.i(90, 0);
            o.str(1, im.path);
            o.d(10, im.w);
            o.d(20, im.h);
            o.d(11, 1);
            o.d(21, 1);
            o.i(280, 1);
            o.i(281, 0);
            o.zero("IMAGEDEF_REACTOR");
            o.h(5, im.reactor);
            o.h(330, im.image);
            o.raw(100, "AcDbRasterImageDefReactor");
            o.i(90, 2);
            o.h(330, im.image);
        }
    }

    if (pdfDict) {
        ownedDict(pdfDict);
        std::set<std::string> used;
        for (const PdfOut& pd : pdfs_) {
            std::string key = cleanName(pd.name, "pdf");
            for (int n = 2; !used.insert(key).second; ++n)
                key = cleanName(pd.name, "pdf") + "_" + std::to_string(n);
            o.str(3, key);
            o.h(350, pd.def);
        }
        for (const PdfOut& pd : pdfs_) {
            o.zero("PDFDEFINITION");
            o.h(5, pd.def);
            o.raw(102, "{ACAD_REACTORS");
            o.h(330, pdfDict);
            o.raw(102, "}");
            o.h(330, pdfDict);
            o.raw(100, "AcDbUnderlayDefinition");
            o.str(1, pd.file);
            o.str(2, pd.page);
        }
    }

    for (const GroupOut& g : groupOut) {
        o.zero("GROUP");
        o.h(5, g.handle);
        o.raw(102, "{ACAD_REACTORS");
        o.h(330, groupDict);
        o.raw(102, "}");
        o.h(330, groupDict);
        o.raw(100, "AcDbGroup");
        o.str(300, g.description);
        o.i(70, !g.name.empty() && g.name[0] == '*' ? 1 : 0);
        o.i(71, g.selectable ? 1 : 0);
        for (std::uint64_t m : g.members)
            o.h(340, m);
    }

    ownedDict(layoutDict);
    if (sheets_.empty()) {
        o.raw(3, "Layout1");
        o.h(350, layoutPaper_);
    }
    for (const Sheet& s : sheets_) {
        o.str(3, s.name);
        o.h(350, s.layout);
    }
    o.raw(3, "Model");
    o.h(350, layoutModel_);

    // Styles other than Standard (a stored STANDARD replaces the built-in one).
    struct StyleOut {
        mline::Style style;
        std::uint64_t handle;
    };
    std::vector<StyleOut> extraStyles;
    mline::Style standard = mline::standardStyle();
    for (const std::string& n : mline::styleNames(doc_)) {
        auto st = mline::findStyle(doc_, n);
        if (!st)
            continue;
        if (n == "STANDARD")
            standard = *st;
        else
            extraStyles.push_back({*st, newHandle()});
    }
    ownedDict(mlineDict);
    o.raw(3, "Standard");
    o.h(350, mlineStyle);
    for (const StyleOut& e : extraStyles) {
        o.str(3, e.style.name);
        o.h(350, e.handle);
    }

    o.zero("ACDBDICTIONARYWDFLT");
    o.h(5, plotDict);
    o.raw(102, "{ACAD_REACTORS");
    o.h(330, rootDict_);
    o.raw(102, "}");
    o.h(330, rootDict_);
    o.raw(100, "AcDbDictionary");
    o.i(281, 1);
    o.raw(3, "Normal");
    o.h(350, placeholder);
    for (const auto& [styleName, handle] : pstyles_) {
        o.str(3, styleName);
        o.h(350, handle);
    }
    o.raw(100, "AcDbDictionaryWithDefault");
    o.h(340, placeholder);

    o.zero("ACDBPLACEHOLDER");
    o.h(5, placeholder);
    o.raw(102, "{ACAD_REACTORS");
    o.h(330, plotDict);
    o.raw(102, "}");
    o.h(330, plotDict);

    for (const auto& [styleName, handle] : pstyles_) {
        (void)styleName;
        o.zero("ACDBPLACEHOLDER");
        o.h(5, handle);
        o.raw(102, "{ACAD_REACTORS");
        o.h(330, plotDict);
        o.raw(102, "}");
        o.h(330, plotDict);
    }

    auto writeStyle = [&](const mline::Style& st, std::uint64_t handle, const char* name) {
        o.zero("MLINESTYLE");
        o.h(5, handle);
        o.raw(102, "{ACAD_REACTORS");
        o.h(330, mlineDict);
        o.raw(102, "}");
        o.h(330, mlineDict);
        o.raw(100, "AcDbMlineStyle");
        if (name)
            o.raw(2, name);
        else
            o.str(2, st.name);
        o.i(70, (st.startCap ? 16 : 0) | (st.endCap ? 256 : 0) | (st.joints ? 2 : 0) |
                    (st.startArc == 1   ? 64
                     : st.startArc == 2 ? 32
                                        : 0) |
                    (st.endArc == 1   ? 1024
                     : st.endArc == 2 ? 512
                                      : 0) |
                    (st.fill >= 0 ? 1 : 0));
        o.str(3, st.description);
        o.i(62, st.fill >= 0 ? st.fill : 256);
        o.d(51, 90);
        o.d(52, 90);
        std::vector<std::size_t> idx(st.offsets.size());
        for (std::size_t i = 0; i < idx.size(); ++i)
            idx[i] = i;
        std::stable_sort(idx.begin(), idx.end(), [&](std::size_t a, std::size_t b) {
            return st.offsets[a] > st.offsets[b];
        });
        o.i(71, static_cast<int>(idx.size()));
        for (std::size_t i : idx) {
            o.d(49, st.offsets[i]);
            o.i(62, st.colorOf(i));
            if (st.linetypeOf(i).empty())
                o.raw(6, "BYLAYER");
            else
                o.str(6, st.linetypeOf(i));
        }
    };
    writeStyle(standard, mlineStyle, "Standard");
    for (const StyleOut& e : extraStyles)
        writeStyle(e.style, e.handle, nullptr);

    // Extension dictionaries of entities: a dictionary per entity, one XRECORD per entry.
    for (const PendingDict& pd : L().pendingDicts) {
        std::vector<std::uint64_t> recHandles;
        for (std::size_t i = 0; i < pd.records.size(); ++i)
            recHandles.push_back(newHandle());
        o.zero("DICTIONARY");
        o.h(5, pd.handle);
        o.raw(102, "{ACAD_REACTORS");
        o.h(330, pd.owner);
        o.raw(102, "}");
        o.h(330, pd.owner);
        o.raw(100, "AcDbDictionary");
        o.i(281, 1);
        for (std::size_t i = 0; i < pd.records.size(); ++i) {
            o.str(3, pd.records[i].name);
            o.h(350, recHandles[i]);
        }
        for (std::size_t i = 0; i < pd.records.size(); ++i) {
            o.zero("XRECORD");
            o.h(5, recHandles[i]);
            o.raw(102, "{ACAD_REACTORS");
            o.h(330, pd.handle);
            o.raw(102, "}");
            o.h(330, pd.handle);
            o.raw(100, "AcDbXrecord");
            o.i(280, 1);
            for (const DataItem& it : pd.records[i].items) {
                switch (dataKindOf(it.code)) {
                    case DataKind::Text:
                        o.str(it.code, it.text);
                        break;
                    case DataKind::Integer:
                        o.i(it.code, it.integer);
                        break;
                    case DataKind::Real:
                        o.d(it.code, it.real);
                        break;
                }
            }
        }
    }

    auto layout = [&](std::uint64_t handle, const std::string& name, int tab, std::uint64_t rec,
                      int flags, double paperW = 0, double paperH = 0) {
        o.zero("LAYOUT");
        o.h(5, handle);
        o.raw(102, "{ACAD_REACTORS");
        o.h(330, layoutDict);
        o.raw(102, "}");
        o.h(330, layoutDict);
        o.raw(100, "AcDbPlotSettings");
        // The page setup the layout came with, while its paper size is still the same.
        bool kept = false;
        if (auto it = keptCustom.find(std::string(kKeptPlotSetPrefix) + name);
            it != keptCustom.end()) {
            const KeptGroups g = unpackGroups(it->second);
            double w = 0, h = 0;
            for (const auto& [code, value] : g) {
                if (code == 44)
                    w = std::strtod(value.c_str(), nullptr);
                else if (code == 45)
                    h = std::strtod(value.c_str(), nullptr);
            }
            const auto near = [](double a, double b) { return std::abs(a - b) < 0.5; };
            if (paperW <= 0 || (near(w, paperW) && near(h, paperH)) ||
                (near(w, paperH) && near(h, paperW))) {
                for (const auto& [code, value] : g)
                    o.str(code, value);
                kept = true;
            }
        }
        if (!kept) {
            o.raw(1, "");
            o.raw(2, "none_device");
            o.raw(4, "");
            o.raw(6, "");
            for (int code : {40, 41, 42, 43, 44, 45, 46, 47, 48, 49})
                o.d(code, code == 44 ? paperW : (code == 45 ? paperH : 0));
            o.d(140, 0);
            o.d(141, 0);
            o.d(142, 1);
            o.d(143, 1);
            o.i(70, flags);
            o.i(72, 0);
            o.i(73, 0);
            o.i(74, 5);
            o.raw(7, "");
            o.i(75, 16);
            o.d(147, 1);
            o.d(148, 0);
            o.d(149, 0);
        }
        o.raw(100, "AcDbLayout");
        o.str(1, name);
        o.i(70, 1);
        o.i(71, tab);
        o.pt2(10, {0, 0});
        o.pt2(11, {paperW > 0 ? paperW : 420, paperH > 0 ? paperH : 297});
        o.pt(12, {0, 0});
        o.pt(14, {0, 0});
        o.pt(15, paperW > 0 ? Vec2{paperW, paperH} : Vec2{0, 0});
        o.d(146, 0);
        o.pt(13, {0, 0});
        o.pt(16, {1, 0});
        o.pt(17, {0, 1});
        o.i(76, 0);
        o.h(330, rec);
    };
    layout(layoutModel_, "Model", 0, modelRec_, 1712);
    if (sheets_.empty()) {
        layout(layoutPaper_, "Layout1", 1, paperRec_, 688);
    } else {
        for (std::size_t i = 0; i < sheets_.size(); ++i)
            layout(sheets_[i].layout, sheets_[i].name, static_cast<int>(i) + 1, sheets_[i].rec, 688,
                   sheets_[i].w, sheets_[i].h);
    }

    // The records kept from the source file.
    for (const auto& [code, value] : keptObjects) {
        if (code == 0) {
            o.zero(value);
        } else if (code == 5 || code == 330 || handleCode(code)) {
            o.h(code, handleOfToken(value));
        } else {
            o.str(code, value);
        }
    }

    o.zero("ENDSEC");
    return std::move(o.buf);
}

std::string Writer::headerSection(std::uint64_t handSeed) {
    Out o(utf8_, r12_);
    o.zero("SECTION");
    o.raw(2, "HEADER");
    auto var = [&](const char* name) { o.raw(9, name); };
    const auto& custom = doc_.header().custom;
    // A variable the source file had (the ones in the default list below are written with the
    // value they had when they differ from the fixed one).
    auto kept = [&](const char* name) {
        auto it = custom.find(std::string(kKeptVarPrefix) + name);
        if (it == custom.end())
            return false;
        const KeptGroups g = unpackGroups(it->second);
        if (g.empty())
            return false;
        var(name);
        for (const auto& [code, value] : g)
            o.str(code, value);
        return true;
    };
    var("$ACADVER");
    o.raw(1, versionCode(opt_.version));
    if (!r12_) {
        var("$HANDSEED");
        o.h(5, handSeed);
    }
    var("$DWGCODEPAGE");
    o.raw(3, "ANSI_1252");
    if (!kept("$INSBASE")) {
        var("$INSBASE");
        o.pt(10, {0, 0});
    }
    if (auto it = doc_.header().custom.find("tbcad.ucs.current");
        it != doc_.header().custom.end()) {
        // the current coordinate system: name TAB x TAB y TAB angle (name empty when unnamed)
        std::vector<std::string> f;
        std::size_t from = 0;
        for (std::size_t t; (t = it->second.find('\t', from)) != std::string::npos; from = t + 1)
            f.push_back(it->second.substr(from, t - from));
        f.push_back(it->second.substr(from));
        if (f.size() == 4) {
            const double x = std::strtod(f[1].c_str(), nullptr),
                         y = std::strtod(f[2].c_str(), nullptr),
                         a = std::strtod(f[3].c_str(), nullptr);
            var("$UCSNAME");
            o.str(2, f[0]);
            var("$UCSORG");
            o.pt(10, {x, y}, 0);
            var("$UCSXDIR");
            o.pt(10, {std::cos(a), std::sin(a)}, 0);
            var("$UCSYDIR");
            o.pt(10, {-std::sin(a), std::cos(a)}, 0);
        }
    }
    var("$EXTMIN");
    o.pt(10, extents_.empty() ? Vec2{1e20, 1e20} : extents_.min, extents_.empty() ? 1e20 : 0);
    var("$EXTMAX");
    o.pt(10, extents_.empty() ? Vec2{-1e20, -1e20} : extents_.max, extents_.empty() ? -1e20 : 0);
    const BBox2 limits = drawingLimits(doc_.header());
    var("$LIMMIN");
    o.pt2(10, limits.empty() ? Vec2{0, 0} : limits.min);
    var("$LIMMAX");
    o.pt2(10, limits.empty() ? Vec2{420, 297} : limits.max);
    if (!colorDependentPlotStyles(doc_.header())) {
        var("$PSTYLEMODE");
        o.i(70, 0);
    }
    var("$QTEXTMODE");
    o.i(70, quickText(doc_.header()) ? 1 : 0);
    var("$FILLMODE");
    o.i(70, fillMode(doc_.header()) ? 1 : 0);
    var("$ATTMODE");
    {
        const int m = attDisplay(doc_.header());
        o.i(70, m == 0 ? 1 : m == 1 ? 2 : 0);
    }
    var("$LTSCALE");
    o.d(40, doc_.header().ltScale);
    var("$CELTSCALE");
    o.d(40, ceLtScale(doc_.header()));
    var("$PSLTSCALE");
    o.i(70, psLtScale(doc_.header()) ? 1 : 0);
    var("$MSLTSCALE");
    o.i(70, msLtScale(doc_.header()) ? 1 : 0);
    var("$PLINEGEN");
    o.i(70, plineGen(doc_.header()) ? 1 : 0);
    var("$MIRRTEXT");
    o.i(70, mirrText(doc_.header()) ? 1 : 0);
    if (!kept("$CECOLOR")) {
        var("$CECOLOR");
        o.i(62, 256);
    }
    if (!kept("$CELTYPE")) {
        var("$CELTYPE");
        o.raw(6, "BYLAYER");
    }
    var("$CLAYER");
    o.str(8, layerNameOf(doc_.header().currentLayer));
    var("$TEXTSTYLE");
    o.str(7, textStyleNameOf(doc_.header().currentTextStyle));
    var("$DIMSTYLE");
    o.str(2, dimStyleNameOf(doc_.header().currentDimStyle));
    if (!kept("$LUNITS")) {
        var("$LUNITS");
        o.i(70, 2);
    }
    if (!kept("$LUPREC")) {
        var("$LUPREC");
        o.i(70, 4);
    }
    if (!kept("$AUNITS")) {
        var("$AUNITS");
        o.i(70, 0);
    }
    if (!kept("$AUPREC")) {
        var("$AUPREC");
        o.i(70, 0);
    }
    if (!r12_) {
        var("$MEASUREMENT");
        o.i(70, 1);
        var("$INSUNITS");
        o.i(70, 4);  // millimetres: every TBCAD length is in mm
        // The unit the drawing shows lengths in, as a note TBCAD reads back (USERI5 is free for
        // applications; AutoCAD ignores the value).
        const LengthUnit shown = doc_.header().units;
        const char* slot = nullptr;  // the first of USERI5..1 the file did not use itself
        for (const char* n : {"$USERI5", "$USERI4", "$USERI3", "$USERI2", "$USERI1"}) {
            const auto it = custom.find(std::string(kKeptVarPrefix) + n);
            bool free = it == custom.end();
            if (!free) {  // a variable the file left at its default is as good as absent
                const KeptGroups g = unpackGroups(it->second);
                free = g.size() == 1 && g[0].first == 70 && g[0].second == "0";
            }
            if (free) {
                slot = n;
                break;
            }
        }
        if (shown != LengthUnit::Millimeter && slot) {
            var(slot);
            o.i(70, 8100 + (shown == LengthUnit::Inch         ? 1
                            : shown == LengthUnit::Foot       ? 2
                            : shown == LengthUnit::Centimeter ? 5
                                                              : 6));
        }
        for (const auto& [key, value] : custom) {
            if (key.compare(0, kKeptVarPrefix.size(), kKeptVarPrefix) != 0)
                continue;
            const std::string name = key.substr(kKeptVarPrefix.size());
            if (ownedHeaderVar(name))
                continue;
            if (shown != LengthUnit::Millimeter && slot && name == slot)
                continue;  // the note about the shown unit took its place
            kept(name.c_str());
        }
    }
    o.zero("ENDSEC");
    return std::move(o.buf);
}

Result<ExportResult> Writer::run() {
    if (opt_.layerSeparator.empty()) {
        return makeError(ErrorCode::InvalidArgument, "the layer separator must not be empty");
    }
    for (char c : opt_.layerSeparator) {
        if (c == '|' || c == '$' || isInvalidNameChar(c)) {
            return makeError(ErrorCode::InvalidArgument,
                             std::string("the character '") + c +
                                 "' cannot be used in a layer separator (DXF reserves it)");
        }
    }

    loadSourceHandles();
    reserveNamedHandles();
    if (!r12_) {
        for (const tbcad::Group& g : readGroups(doc_))
            for (EntityId m : g.members)
                groupMembers_.insert(m.value);
    }
    prepareNames();
    prepareBlocks();
    modelRec_ = newHandle();
    paperRec_ = newHandle();
    rootDict_ = newHandle();
    layoutModel_ = newHandle();
    layoutPaper_ = newHandle();
    prepareSheets();
    {
        constexpr std::string_view kPrefix = "tbcad.pstyle.ent:";
        const auto& custom = doc_.header().custom;
        auto it = custom.lower_bound(std::string(kPrefix));
        anyEntityPlotStyle_ =
            it != custom.end() && it->first.compare(0, kPrefix.size(), kPrefix) == 0;
    }
    // Extents of model space for $EXTMIN/$EXTMAX and the active viewport.
    std::vector<EntityId> model = doc_.modelSpace();
    if (opt_.onlyLevel) {
        model.erase(std::remove_if(model.begin(), model.end(),
                                   [&](EntityId id) {
                                       const Entity* e = doc_.find(id);
                                       return !e || e->common.level != opt_.onlyLevel;
                                   }),
                    model.end());
    }
    {
        auto extentsOf = [&](std::size_t from, std::size_t to) {
            BBox2 acc;
            text::FontScope fonts;  // text bounds are laid out without waiting for other threads
            for (std::size_t i = from; i < to; ++i) {
                const Entity* e = doc_.find(model[i]);
                if (!e)
                    continue;
                BBox2 b = geom::bounds(*e, doc_);
                if (const auto* t = entityAs<TextData>(*e)) {
                    b.expand(t->position);
                    b.expand(t->alignPoint);
                } else if (const auto* m = entityAs<MTextData>(*e)) {
                    b.expand(m->position);
                } else if (const auto* d = entityAs<DimensionData>(*e)) {
                    for (Vec2 p : d->defPts)
                        b.expand(p);
                    b.expand(d->textPos);
                }
                acc.merge(b);
            }
            return acc;
        };
        constexpr std::size_t kMinPerLane = 16384;
        const std::size_t lanes = std::clamp<std::size_t>(
            std::min<std::size_t>(std::thread::hardware_concurrency(), model.size() / kMinPerLane),
            1, 16);
        if (lanes < 2) {
            extents_.merge(extentsOf(0, model.size()));
        } else {
            // The lookups that fill a cache on first use are done before the threads start.
            for (const auto& [id, layer] : doc_.layers())
                (void)doc_.effective(id);
            std::vector<std::future<BBox2>> jobs;
            for (std::size_t k = 1; k < lanes; ++k)
                jobs.push_back(std::async(std::launch::async, extentsOf, model.size() * k / lanes,
                                          model.size() * (k + 1) / lanes));
            extents_.merge(extentsOf(0, model.size() / lanes));
            for (auto& j : jobs)
                extents_.merge(j.get());
        }
    }

    // Entities first (they decide which dimension blocks exist), then the sections in file order.
    Out entities(utf8_, r12_);
    L().o = &entities;
    writeEntityList(model, modelRec_);
    if (r12_ && !sheets_.empty()) {
        // R12 has one paper space, kept among the entities with group 67: the first layout's
        // objects go there; its viewports and the other layouts have no R12 form.
        if (const BlockDef* def = doc_.block(sheets_[0].content))
            writeEntityList(def->entities, paperRec_);
        bool viewports = false;
        for (const Sheet& s : sheets_)
            viewports = viewports || !s.viewports.empty();
        if (sheets_.size() > 1 || viewports)
            downgrade("LAYOUT",
                      "R12 has one paper space: only the first layout's objects are "
                      "written, without viewports");
    }

    std::string blocks;
    {
        Out o(utf8_, r12_);
        o.zero("SECTION");
        o.raw(2, "BLOCKS");
        blockBegin(o, "*Model_Space", modelRec_, {0, 0}, 0, false);
        blockEnd(o, modelRec_, false);
        if (sheets_.empty() || r12_) {
            blockBegin(o, "*Paper_Space", paperRec_, {0, 0}, 0, true);
            blockEnd(o, paperRec_, true);
        }
        for (BlockId id : userBlocks_) {
            const BlockDef& def = *doc_.block(id);
            std::uint64_t rec = blockRec_[id.value];
            bool anon = blockName_[id.value][0] == '*';
            blockBegin(o, blockName_[id.value], rec, def.base, anon ? 1 : 0, false, &def);
            Out* saved = L().o;
            L().o = &o;
            writeEntityList(def.entities, rec);
            L().o = saved;
            blockEnd(o, rec, false);
        }
        // The sheets come after the blocks they insert, so an INSERT finds its definition.
        for (std::size_t i = 0; !r12_ && i < sheets_.size(); ++i)
            writeSheetBlock(o, sheets_[i],
                            i == 0 ? "*Paper_Space" : "*Paper_Space" + std::to_string(i - 1));
        // Dimension blocks may be added while the blocks above are written, so index.
        for (std::size_t i = 0; i < dimBlocks_.size(); ++i) {
            const DimBlock& b = dimBlocks_[i];
            blockBegin(o, b.name, b.rec, {0, 0}, 1, false);
            o.buf += b.body;
            blockEnd(o, b.rec, false);
        }
        o.zero("ENDSEC");
        blocks = std::move(o.buf);
    }

    std::string tables = tablesSection();
    std::string objects = r12_ ? std::string() : objectsSection();

    // Everything before the entities goes into `out` first; the entities, usually most of the file,
    // are then put behind it without a second full copy when their buffer has room (a 1M-entity
    // drawing would otherwise hold its DXF text twice).
    std::string out;
    out.reserve(blocks.size() + tables.size() + (1u << 20));
    out += headerSection(handleSeed());
    if (!r12_) {
        out += "  0\r\nSECTION\r\n  2\r\nCLASSES\r\n";
        if (!images_.empty()) {
            out +=
                "  0\r\nCLASS\r\n  1\r\nIMAGE\r\n  2\r\nAcDbRasterImage\r\n  3\r\nISM\r\n"
                " 90\r\n127\r\n280\r\n0\r\n281\r\n1\r\n";
            out +=
                "  0\r\nCLASS\r\n  1\r\nIMAGEDEF\r\n  2\r\nAcDbRasterImageDef\r\n  3\r\nISM\r\n"
                " 90\r\n0\r\n280\r\n0\r\n281\r\n0\r\n";
            out +=
                "  0\r\nCLASS\r\n  1\r\nIMAGEDEF_REACTOR\r\n  2\r\nAcDbRasterImageDefReactor\r\n"
                "  3\r\nISM\r\n 90\r\n1\r\n280\r\n0\r\n281\r\n0\r\n";
            out +=
                "  0\r\nCLASS\r\n  1\r\nRASTERVARIABLES\r\n  2\r\nAcDbRasterVariables\r\n"
                "  3\r\nISM\r\n 90\r\n0\r\n280\r\n0\r\n281\r\n0\r\n";
        }
        if (!pdfs_.empty()) {
            out +=
                "  0\r\nCLASS\r\n  1\r\nPDFUNDERLAY\r\n  2\r\nAcDbUnderlayReference\r\n"
                "  3\r\nObjectDBX Classes\r\n 90\r\n127\r\n280\r\n0\r\n281\r\n1\r\n";
            out +=
                "  0\r\nCLASS\r\n  1\r\nPDFDEFINITION\r\n  2\r\nAcDbUnderlayDefinition\r\n"
                "  3\r\nObjectDBX Classes\r\n 90\r\n0\r\n280\r\n0\r\n281\r\n0\r\n";
        }
        if (wipeouts_)
            out +=
                "  0\r\nCLASS\r\n  1\r\nWIPEOUT\r\n  2\r\nAcDbWipeout\r\n"
                "  3\r\nWipeOut\r\n 90\r\n127\r\n280\r\n0\r\n281\r\n1\r\n";
        if (!r12_ && (wipeouts_ || doc_.header().custom.count(kWipeoutFrameKey)))
            out +=
                "  0\r\nCLASS\r\n  1\r\nWIPEOUTVARIABLES\r\n  2\r\nAcDbWipeoutVariables\r\n"
                "  3\r\nWipeOut\r\n 90\r\n0\r\n280\r\n0\r\n281\r\n0\r\n";
        if (auto it = doc_.header().custom.find(kKeptClassesKey);
            it != doc_.header().custom.end()) {
            Out c(utf8_, false);
            for (const auto& [code, value] : unpackGroups(it->second))
                c.str(code, value);
            out += c.buf;
        }
        out += "  0\r\nENDSEC\r\n";
    }
    out += tables;
    std::string().swap(tables);
    out += blocks;
    std::string().swap(blocks);
    out += "0\r\nSECTION\r\n2\r\nENTITIES\r\n";
    std::string tail = "0\r\nENDSEC\r\n" + objects + "0\r\nEOF\r\n";
    std::string().swap(objects);
    std::string& body = entities.buf;
    if (body.capacity() - body.size() >= out.size() + tail.size()) {
        body.insert(0, out);
        out = std::move(body);
    } else {
        out.reserve(out.size() + body.size() + tail.size());
        out += body;
    }
    std::string().swap(body);
    out += tail;

    std::size_t bad = entities.badNumbers;
    if (bad > 0) {
        rep_.warn(wcode::kBadNumber,
                  std::to_string(bad) + " non-finite coordinates were written as 0");
    }

    ExportResult result;
    result.bytes = std::move(out);
    for (const auto& [type, count] : main_.counts)
        summary_.entities[type] += count;
    summary_.acadVer = std::string(versionCode(opt_.version));
    summary_.version = opt_.version == DxfVersion::R12
                           ? "R12"
                           : (opt_.version == DxfVersion::R2000 ? "R2000" : "R2007");
    summary_.utf8 = utf8_;
    summary_.layers = doc_.layers().size();
    summary_.blocks = userBlocks_.size() + dimBlocks_.size();
    result.summary = std::move(summary_);
    return result;
}

}  // namespace tbcad::dxf::detail

namespace tbcad::dxf {

std::string_view versionCode(DxfVersion v) {
    switch (v) {
        case DxfVersion::R12:
            return "AC1009";
        case DxfVersion::R2000:
            return "AC1015";
        case DxfVersion::R2007:
            return "AC1021";
    }
    return "AC1015";
}

std::optional<DxfVersion> parseVersion(std::string_view text) {
    std::string t(text);
    for (char& c : t) {
        if (c >= 'A' && c <= 'Z')
            c = static_cast<char>(c - 'A' + 'a');
    }
    if (t == "r12" || t == "12" || t == "ac1009" || t == "1009")
        return DxfVersion::R12;
    if (t == "r2000" || t == "2000" || t == "ac1015" || t == "1015" || t == "r15")
        return DxfVersion::R2000;
    if (t == "r2007" || t == "2007" || t == "ac1021" || t == "1021" || t == "r21")
        return DxfVersion::R2007;
    return std::nullopt;
}

std::string ExportSummary::toJson(const Report& report) const {
    std::string o = "{\"output\":{\"acadver\":\"" + acadVer + "\",\"version\":\"" + version +
                    "\",\"utf8\":" + (utf8 ? "true" : "false") + "},\"entities\":{";
    bool first = true;
    for (const auto& [type, n] : entities) {
        if (!first)
            o += ",";
        first = false;
        o += "\"" + type + "\":" + std::to_string(n);
    }
    o += "},\"summary\":{\"layers\":" + std::to_string(layers) +
         ",\"blocks\":" + std::to_string(blocks) + ",\"skipped\":" + std::to_string(skipped) +
         ",\"downgraded\":" + std::to_string(downgraded) + "},\"report\":" + report.toJson() + "}";
    return o;
}

Result<ExportResult> exportDxf(const Document& doc, const ExportOptions& options, Report& report) {
    detail::Writer w(doc, options, report);
    return w.run();
}

}  // namespace tbcad::dxf
