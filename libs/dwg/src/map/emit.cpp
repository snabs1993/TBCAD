#include "map/emit.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <unordered_map>
#include <vector>

#include "map/dxf_text.h"
#include "objects/decode.h"

namespace tbcad::dwg::detail {

namespace {

std::string hexHandle(std::uint64_t h) {
    char buf[24];
    std::snprintf(buf, sizeof buf, "%llX", static_cast<unsigned long long>(h));
    return buf;
}

bool iequals(const std::string& a, const char* b) {
    const std::string_view bv(b);
    if (a.size() != bv.size())
        return false;
    for (std::size_t i = 0; i < a.size(); ++i)
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(bv[i])))
            return false;
    return true;
}

void emitGroup(DxfText& o, const Group& g) {
    std::visit(
        [&](const auto& v) {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, long long>) {
                o.group(g.code, v);
            } else if constexpr (std::is_same_v<T, double>) {
                o.group(g.code, v);
            } else {
                o.group(g.code, v);
            }
        },
        g.value.v);
}

struct Names {
    const Drawing& d;
    std::string layer(std::uint64_t h) const {
        auto it = d.layers.find(h);
        return it == d.layers.end() ? "0" : it->second.name;
    }
    std::string linetype(std::uint64_t h) const {
        auto it = d.linetypes.find(h);
        return it == d.linetypes.end() ? std::string{} : it->second.name;
    }
    std::string style(std::uint64_t h) const {
        auto it = d.styles.find(h);
        return it == d.styles.end() ? std::string{} : it->second.name;
    }
    std::string dimStyle(std::uint64_t h) const {
        auto it = d.dimStyles.find(h);
        return it == d.dimStyles.end() ? std::string{} : it->second.name;
    }
    std::string block(std::uint64_t h) const {
        auto it = d.blocks.find(h);
        return it == d.blocks.end() ? std::string{} : it->second.name;
    }
};

// The clipping boundary of an INSERT (XCLIP): its extension dictionary holds ACAD_FILTER, whose
// SPATIAL entry is the SPATIAL_FILTER object.
const SpatialFilterRec* spatialFilterOf(const Drawing& d, const EntityRec& e) {
    if (e.xdic == 0 || d.spatialFilters.empty())
        return nullptr;
    auto entry = [&](std::uint64_t dict, const char* name) -> std::uint64_t {
        auto it = d.dictionaries.find(dict);
        if (it == d.dictionaries.end())
            return 0;
        for (const auto& [nm, h] : it->second)
            if (iequals(nm, name))
                return h;
        return 0;
    };
    const std::uint64_t filters = entry(e.xdic, "ACAD_FILTER");
    const std::uint64_t spatial = filters ? entry(filters, "SPATIAL") : 0;
    auto f = d.spatialFilters.find(spatial);
    return f == d.spatialFilters.end() ? nullptr : &f->second;
}

// Box over the points of the entities a block owns (group codes 10..13 with their 20..23).
bool blockExtents(const Drawing& d, std::uint64_t blockHeader, double box[4]) {
    auto b = d.blocks.find(blockHeader);
    if (b == d.blocks.end())
        return false;
    bool any = false;
    for (std::uint64_t h : b->second.members) {
        auto it =
            std::lower_bound(d.entities.begin(), d.entities.end(), h,
                             [](const EntityRec& a, std::uint64_t v) { return a.handle < v; });
        if (it == d.entities.end() || it->handle != h)
            continue;
        double xs[4] = {0, 0, 0, 0};
        bool haveX[4] = {false, false, false, false};
        for (const Group& g : it->groups) {
            const int c = g.code;
            auto num = [&] {
                return std::holds_alternative<double>(g.value.v)
                           ? std::get<double>(g.value.v)
                           : static_cast<double>(std::holds_alternative<long long>(g.value.v)
                                                     ? std::get<long long>(g.value.v)
                                                     : 0);
            };
            if (c >= 10 && c <= 13) {
                xs[c - 10] = num();
                haveX[c - 10] = true;
            } else if (c >= 20 && c <= 23 && haveX[c - 20]) {
                const double x = xs[c - 20], y = num();
                if (!any) {
                    box[0] = box[2] = x;
                    box[1] = box[3] = y;
                    any = true;
                } else {
                    box[0] = std::min(box[0], x);
                    box[1] = std::min(box[1], y);
                    box[2] = std::max(box[2], x);
                    box[3] = std::max(box[3], y);
                }
                haveX[c - 20] = false;
            }
        }
    }
    return any;
}

// The clip as the importer reads it back (TBCAD extended data: on/off, then the corners in block
// coordinates). The corners are taken as stored; a boundary that does not touch the block at all
// is dropped rather than hiding the whole block.
void emitClip(DxfText& o, const EntityRec& e, const Names& n) {
    const SpatialFilterRec* f = spatialFilterOf(n.d, e);
    if (!f || f->pts.size() < 3)
        return;
    double bx[4];
    if (blockExtents(n.d, e.blockHeader, bx)) {
        double px0 = f->pts[0][0], py0 = f->pts[0][1], px1 = px0, py1 = py0;
        for (const auto& p : f->pts) {
            px0 = std::min(px0, p[0]), px1 = std::max(px1, p[0]);
            py0 = std::min(py0, p[1]), py1 = std::max(py1, p[1]);
        }
        if (px1 < bx[0] || px0 > bx[2] || py1 < bx[1] || py0 > bx[3])
            return;
    }
    o.group(1001, "TBCAD");
    o.group(1000, f->displayBoundary ? "clip=1" : "clip=0");
    for (const auto& p : f->pts) {
        o.group(1010, p[0]);
        o.group(1020, p[1]);
    }
}

void emitEntity(DxfText& o, const EntityRec& e, const Names& n) {
    o.begin(e.dxfType);
    o.group(8, n.layer(e.layer));
    if (e.ltFlags == 3 && e.linetype) {
        const std::string lt = n.linetype(e.linetype);
        if (!lt.empty())
            o.group(6, lt);
    } else if (e.ltFlags == 1) {
        o.group(6, "BYBLOCK");
    } else if (e.ltFlags == 2) {
        o.group(6, "CONTINUOUS");
    }
    if (e.color != 256)
        o.group(62, e.color);
    if (e.trueColor >= 0)
        o.group(420, e.trueColor);
    if (e.transparency != 0)
        o.group(440, e.transparency);
    const int lw = lineweightToDxf(e.lineweight);
    if (lw != -3)
        o.group(370, lw);
    if (e.ltScale != 1.0)
        o.group(48, e.ltScale);
    if (e.invisible)
        o.group(60, 1);
    if (e.entmode == 1)
        o.group(67, 1);
    if (e.dxfType == "INSERT") {
        o.group(2, n.block(e.blockHeader));
        if (e.hasChildren && (e.firstChild || !e.childHandles.empty()))
            o.group(66, 1);
    }
    if (e.dxfType == "POLYLINE")
        o.group(66, 1);
    if (e.dxfType == "DIMENSION") {
        o.group(2, n.block(e.blockHeader));
        o.group(3, n.dimStyle(e.dimStyle));
    }
    if (e.dxfType == "LEADER")
        o.group(3, n.dimStyle(e.dimStyle));
    if (e.dxfType == "TEXT" || e.dxfType == "ATTRIB" || e.dxfType == "ATTDEF" ||
        e.dxfType == "MTEXT") {
        const std::string st = n.style(e.style);
        if (!st.empty())
            o.group(7, st);
    }
    for (const Group& g : e.groups)
        emitGroup(o, g);
    if (e.dxfType == "INSERT")
        emitClip(o, e, n);
    if (e.dxfType == "VIEWPORT" && !e.frozenLayers.empty()) {
        // The layers frozen in this viewport, by name (read back by the importer).
        o.group(1001, "TBCAD");
        for (std::uint64_t h : e.frozenLayers)
            o.group(1000, "frozen=" + n.layer(h));
    }
}

bool isChild(const EntityRec& e) {
    return e.dxfType == "VERTEX" || e.dxfType == "ATTRIB";
}

// Writes the entity and, for a polyline or an insert with attributes, its owned records (found
// by handle range, they follow each other) and the closing SEQEND.
void emitWithChildren(DxfText& o, const Drawing& d, const EntityRec& e, const Names& n) {
    emitEntity(o, e, n);
    const bool poly = e.dxfType == "POLYLINE";
    const bool listed = !e.childHandles.empty();
    if (!poly && !(e.dxfType == "INSERT" && e.hasChildren && (e.firstChild || listed)))
        return;
    const char* want = poly ? "VERTEX" : "ATTRIB";
    auto byHandle = [&](std::uint64_t h) {
        return std::lower_bound(d.entities.begin(), d.entities.end(), h,
                                [](const EntityRec& a, std::uint64_t v) { return a.handle < v; });
    };
    if (listed) {
        for (std::uint64_t h : e.childHandles) {
            auto it = byHandle(h);
            if (it != d.entities.end() && it->handle == h && it->dxfType == want)
                emitEntity(o, *it, n);
        }
    } else {
        for (auto it = byHandle(e.firstChild); it != d.entities.end() && it->handle <= e.lastChild;
             ++it)
            if (it->dxfType == want)
                emitEntity(o, *it, n);
    }
    o.begin("SEQEND");
    o.group(8, n.layer(e.layer));
}

}  // namespace

std::string emitDxf(const Drawing& d) {
    DxfText o;
    const Names n{d};

    o.beginSection("HEADER");
    o.group(9, "$ACADVER");
    o.group(1, d.unicode ? "AC1021" : "AC1015");
    if (!d.unicode) {
        o.group(9, "$DWGCODEPAGE");
        o.group(3, "ANSI_1252");
    }
    if (d.unitsKnown) {
        o.group(9, "$INSUNITS");
        o.group(70, d.insunits);
    }
    if (d.ltscale > 0 && d.ltscale != 1.0) {
        o.group(9, "$LTSCALE");
        o.group(40, d.ltscale);
    }
    o.endSection();

    // Which block header is model space; entities owned by it (or with entity mode 2) are drawn.
    std::uint64_t modelSpace = 0, paperSpace = 0;
    for (const auto& [h, b] : d.blocks) {
        if (iequals(b.name, "*Model_Space"))
            modelSpace = h;
        else if (iequals(b.name, "*Paper_Space"))
            paperSpace = h;
    }

    o.beginSection("TABLES");
    o.begin("TABLE");
    o.group(2, "LTYPE");
    o.group(70, static_cast<long long>(d.linetypes.size()));
    for (const auto& [h, t] : d.linetypes) {
        o.begin("LTYPE");
        o.group(2, t.name);
        o.group(70, 0);
        o.group(3, t.description);
        o.group(72, 65);
        o.group(73, static_cast<long long>(t.dashes.size()));
        o.group(40, t.patternLength);
        for (double dash : t.dashes)
            o.group(49, dash);
    }
    o.begin("ENDTAB");
    o.begin("TABLE");
    o.group(2, "LAYER");
    o.group(70, static_cast<long long>(d.layers.size()));
    for (const auto& [h, l] : d.layers) {
        o.begin("LAYER");
        o.group(2, l.name);
        o.group(70, l.flags);
        o.group(62, l.color == 0 ? 7 : l.color);
        if (l.trueColor >= 0)
            o.group(420, l.trueColor);
        if (l.transparency != 0)
            o.group(440, l.transparency);
        const std::string lt = n.linetype(l.linetype);
        o.group(6, lt.empty() ? "CONTINUOUS" : lt);
        if (!l.plot)
            o.group(290, 0);
        if (l.lineweight != -3)
            o.group(370, l.lineweight);
        if (!l.description.empty()) {
            o.group(1001, "AcAecLayerStandard");
            o.group(1000, "");
            o.group(1000, l.description);
        }
    }
    o.begin("ENDTAB");
    o.begin("TABLE");
    o.group(2, "STYLE");
    o.group(70, static_cast<long long>(d.styles.size()));
    for (const auto& [h, s] : d.styles) {
        o.begin("STYLE");
        o.group(5, hexHandle(h));
        o.group(2, s.name);
        o.group(70, s.flags);
        o.group(40, s.height);
        o.group(41, s.width);
        o.group(50, s.oblique * 180.0 / 3.14159265358979323846);
        o.group(71, s.generation);
        o.group(42, s.lastHeight);
        o.group(3, s.font);
        o.group(4, s.bigFont);
    }
    o.begin("ENDTAB");
    o.begin("TABLE");
    o.group(2, "DIMSTYLE");
    o.group(70, static_cast<long long>(d.dimStyles.size()));
    for (const auto& [h, ds] : d.dimStyles) {
        o.begin("DIMSTYLE");
        o.group(2, ds.name);
        o.group(70, ds.flags);
        if (ds.textStyle != 0)
            o.group(340, hexHandle(ds.textStyle));
        // The arrow blocks go out by name (the built-in ones are _ARCHTICK, _DOT, ...), so the
        // importer can pick the arrow kind without a block record table.
        auto arrowName = [&](std::uint64_t handle) {
            const auto b = d.blocks.find(handle);
            return b == d.blocks.end() ? std::string{} : b->second.name;
        };
        if (const std::string n = arrowName(ds.arrow); !n.empty())
            o.group(342, n);
        if (const std::string n = arrowName(ds.arrow1); !n.empty())
            o.group(343, n);
        if (const std::string n = arrowName(ds.arrow2); !n.empty())
            o.group(344, n);
        if (const std::string n = arrowName(ds.leaderArrow); !n.empty())
            o.group(341, n);
        for (const Group& g : ds.groups)
            emitGroup(o, g);
    }
    o.begin("ENDTAB");
    o.endSection();

    std::unordered_map<std::uint64_t, std::vector<std::size_t>> byOwner;
    for (std::size_t i = 0; i < d.entities.size(); ++i)
        if (d.entities[i].entmode == 0)
            byOwner[d.entities[i].owner].push_back(i);

    // Draw order: entities of a block with a sort table sort by their sort handle (their own
    // handle when not listed); the sort is stable, so the file order breaks ties.
    auto sortByTable = [&](std::uint64_t block, std::vector<std::size_t>& idx) {
        const auto table = d.sortKeys.find(block);
        if (table == d.sortKeys.end())
            return;
        auto key = [&](std::size_t i) {
            const std::uint64_t h = d.entities[i].handle;
            const auto k = table->second.find(h);
            return k == table->second.end() ? h : k->second;
        };
        std::stable_sort(idx.begin(), idx.end(),
                         [&](std::size_t a, std::size_t b) { return key(a) < key(b); });
    };
    for (auto& [owner, idx] : byOwner)
        sortByTable(owner, idx);

    // Layout sheets (paper space blocks) in tab order, then by handle.
    std::vector<std::uint64_t> sheets;
    std::unordered_map<std::uint64_t, const LayoutRec*> layoutOf;
    for (const LayoutRec& l : d.layoutObjects)
        layoutOf[l.block] = &l;
    for (const auto& [h, b] : d.blocks)
        if (b.name.rfind("*Paper_Space", 0) == 0 || b.name.rfind("*PAPER_SPACE", 0) == 0)
            sheets.push_back(h);
    std::stable_sort(sheets.begin(), sheets.end(), [&](std::uint64_t a, std::uint64_t b) {
        const auto la = layoutOf.find(a), lb = layoutOf.find(b);
        const int ta = la == layoutOf.end() ? 1 << 30 : la->second->tab;
        const int tb = lb == layoutOf.end() ? 1 << 30 : lb->second->tab;
        return ta != tb ? ta < tb : a < b;
    });
    auto isSheet = [&](std::uint64_t h) {
        return std::find(sheets.begin(), sheets.end(), h) != sheets.end();
    };

    o.beginSection("BLOCKS");
    for (const auto& [h, b] : d.blocks) {
        if (h == modelSpace || h == paperSpace || isSheet(h))
            continue;
        o.begin("BLOCK");
        o.group(8, "0");
        o.group(2, b.name);
        o.group(70, b.flags);
        o.group(10, b.baseX);
        o.group(20, b.baseY);
        o.group(30, b.baseZ);
        o.group(3, b.name);
        o.group(1, b.xrefPath);
        auto members = byOwner.find(h);
        if (members != byOwner.end())
            for (std::size_t idx : members->second)
                if (!isChild(d.entities[idx]))
                    emitWithChildren(o, d, d.entities[idx], n);
        o.begin("ENDBLK");
        o.group(8, "0");
    }
    for (std::size_t i = 0; i < sheets.size(); ++i) {
        o.begin("BLOCK");
        o.group(8, "0");
        o.group(2, i == 0 ? std::string("*Paper_Space") : "*Paper_Space" + std::to_string(i - 1));
        o.group(70, 0);
        o.group(10, 0.0);
        o.group(20, 0.0);
        o.group(30, 0.0);
        auto members = byOwner.find(sheets[i]);
        if (members != byOwner.end())
            for (std::size_t idx : members->second)
                if (!isChild(d.entities[idx]))
                    emitWithChildren(o, d, d.entities[idx], n);
        o.begin("ENDBLK");
        o.group(8, "0");
    }
    o.endSection();

    o.beginSection("ENTITIES");
    std::vector<std::size_t> modelOrder;
    for (std::size_t i = 0; i < d.entities.size(); ++i) {
        const EntityRec& e = d.entities[i];
        const bool model = e.entmode == 2 || (e.entmode == 0 && e.owner == modelSpace);
        if (model && !isChild(e))
            modelOrder.push_back(i);
    }
    sortByTable(modelSpace, modelOrder);
    for (std::size_t i : modelOrder)
        emitWithChildren(o, d, d.entities[i], n);
    o.endSection();
    if (!sheets.empty()) {
        o.beginSection("OBJECTS");
        for (std::size_t i = 0; i < sheets.size(); ++i) {
            const auto it = layoutOf.find(sheets[i]);
            o.begin("LAYOUT");
            o.group(1, it != layoutOf.end() && !it->second->name.empty()
                           ? it->second->name
                           : "Layout" + std::to_string(i + 1));
            o.group(71, static_cast<long long>(i + 1));
            if (it != layoutOf.end() && !it->second->styleSheet.empty())
                o.group(7, it->second->styleSheet);
            if (it != layoutOf.end() && it->second->width > 0 && it->second->height > 0) {
                o.group(44, it->second->width);
                o.group(45, it->second->height);
                o.group(73, static_cast<long long>(it->second->rotation));
                const double* ex = it->second->extents;
                if (ex[3] > ex[0] && ex[4] > ex[1]) {
                    // The paper rectangle in paper space coordinates.
                    o.group(14, ex[0]);
                    o.group(24, ex[1]);
                    o.group(15, ex[3]);
                    o.group(25, ex[4]);
                }
            }
        }
        o.endSection();
    }
    o.endFile();
    return o.take();
}

}  // namespace tbcad::dwg::detail
