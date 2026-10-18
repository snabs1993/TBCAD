// DXF import: one record -> TBCAD entities (everything except HATCH and DIMENSION).
#include <algorithm>
#include <cmath>

#include "importer.h"
#include "tbcad/gdt/gdt.h"
#include "tbcad/geom/spline.h"
#include "tbcad/mline/mline.h"
#include "tbcad/table/table.h"
#include "tbcad/underlay/image.h"

namespace tbcad::dxf::detail {

namespace {

constexpr double kDeg = kPi / 180.0;

std::string foldAscii(std::string_view s) {
    std::string r(s);
    for (char& c : r) {
        if (c >= 'A' && c <= 'Z')
            c = static_cast<char>(c - 'A' + 'a');
    }
    return r;
}

template <class T>
Entity makeEntity(T data) {
    Entity e;
    e.data = std::move(data);
    return e;
}

std::optional<std::uint64_t> handleOf(const Record& r) {
    if (const Group* g = r.find(5)) {
        if (const auto* h = std::get_if<Handle>(&g->value)) {
            if (h->valid)
                return h->value;
        }
    }
    return std::nullopt;
}

}  // namespace

void Importer::bad(const Record& r, std::string_view type, const std::string& message) {
    report_.warn(code::kBadEntity, std::string(type) + ": " + message, r.line, handleOf(r));
}

// ---------------------------------------------------------------------------------------------
// Layers, linetypes, styles, common properties

LayerId Importer::layerFor(std::string_view raw, int line) {
    if (raw.empty())
        return kLayerZero;
    std::string key = foldAscii(raw);
    auto it = layerByRaw_.find(key);
    if (it != layerByRaw_.end())
        return it->second;
    std::string name = dec(raw);
    auto fit = layerByFold_.find(foldAscii(name));
    if (fit != layerByFold_.end()) {
        layerByRaw_[key] = fit->second;
        return fit->second;
    }
    // Not in the layer table: create it so the entity keeps its layer.
    Layer l;
    l.name = name;
    tbcad::Result<LayerId> made = tx_->addLayer(std::move(l));
    LayerId id = kLayerZero;
    if (made) {
        id = made.value();
        report_.warn(code::kMissingLayer,
                     "layer \"" + name +
                         "\" is used but missing from the layer table; created "
                         "with default properties",
                     line);
    } else {
        report_.warn(code::kMissingLayer,
                     "layer \"" + name + "\" is used but missing and cannot be created (" +
                         made.error().message + "); using layer 0",
                     line);
    }
    layerByRaw_[key] = id;
    layerByFold_[foldAscii(name)] = id;
    return id;
}

LinetypeId Importer::linetypeFor(std::string_view raw, int line) {
    if (raw.empty())
        return kLinetypeByLayer;
    std::string key = foldAscii(raw);
    if (key == "bylayer")
        return kLinetypeByLayer;
    if (key == "byblock")
        return kLinetypeByBlock;
    if (key == "continuous")
        return kLinetypeContinuous;
    auto it = ltypeByRaw_.find(key);
    if (it != ltypeByRaw_.end())
        return it->second;
    if (warnedMissing_.insert("lt:" + key).second) {
        report_.warn(code::kMissingLinetype,
                     "linetype \"" + dec(raw) + "\" is used but not defined; using ByLayer", line);
    }
    return kLinetypeByLayer;
}

TextStyleId Importer::textStyleFor(std::string_view raw) const {
    if (raw.empty())
        return TextStyleId{};
    const TextStyle* s = doc_->textStyleByName(dec(raw));
    return s ? s->id : TextStyleId{};
}

DimStyleId Importer::dimStyleFor(std::string_view raw) const {
    if (raw.empty())
        return DimStyleId{};
    const DimStyle* s = doc_->dimStyleByName(dec(raw));
    return s ? s->id : DimStyleId{};
}

// Group 440: 0 or absent = ByLayer, 0x01000000 = ByBlock, 0x02000000 | alpha (0 clear, 255 opaque).
// Returns percent transparent (0 to 90), or the ByLayer / ByBlock markers.
int transparencyFromGroup(std::int64_t v) {
    if ((v & 0xFF000000) == 0x01000000)
        return kTransparencyByBlock;
    if ((v & 0xFF000000) != 0x02000000)
        return kTransparencyByLayer;
    const int alpha = static_cast<int>(v & 0xFF);
    const int percent = static_cast<int>(std::lround(100.0 * (255 - alpha) / 255.0));
    return std::clamp(percent, 0, static_cast<int>(kTransparencyMax));
}

// Marks an extension dictionary whose records are filled in once the OBJECTS section is read; the
// handle is the first item.
constexpr const char* kPendingDict = "\x01xdict";

// XDATA of applications other than ours and the extension dictionary handle of an entity. TBCAD's
// own XDATA is read by the entity readers, and ACAD's holds data those readers interpret.
ExtData Importer::extOf(const Record& r) {
    ExtData x;
    XdataApp* cur = nullptr;
    for (std::size_t i = 0; i < r.g.size(); ++i) {
        const Group& g = r.g[i];
        if (g.code == 102 && g.str() == "{ACAD_XDICTIONARY" && i + 1 < r.g.size() &&
            r.g[i + 1].code == 360) {
            DictRecord pending;
            pending.name = kPendingDict;
            pending.items.push_back(DataItem{360, std::string(r.g[i + 1].str()), 0, 0});
            x.dict.push_back(std::move(pending));
            continue;
        }
        if (g.code == 1001) {
            const std::string app = dec(g.str());
            if (app == "TBCAD" || app == "ACAD" || x.xdata.size() >= 256) {
                cur = nullptr;
            } else {
                x.xdata.push_back(XdataApp{app, {}});
                cur = &x.xdata.back();
            }
            continue;
        }
        if (!cur || g.code < 1000 || g.code > 1071 || cur->items.size() >= 4096)
            continue;
        DataItem it;
        it.code = g.code;
        switch (dataKindOf(g.code)) {
            case DataKind::Text:
                it.text = dec(g.str());
                break;
            case DataKind::Integer:
                it.integer = g.integer();
                break;
            case DataKind::Real:
                it.real = g.number();
                if (!std::isfinite(it.real))
                    it.real = 0;
                break;
        }
        cur->items.push_back(std::move(it));
    }
    return x;
}

// Fills the pending extension dictionaries of all entities from the OBJECTS section.
void Importer::resolveExtDictionaries() {
    std::vector<EntityId> todo;
    doc_->forEachEntity([&](const Entity& e) {
        for (const DictRecord& d : e.common.ext.get().dict)
            if (d.name == kPendingDict) {
                todo.push_back(e.common.id);
                break;
            }
    });
    for (EntityId id : todo) {
        (void)tx_->modify(id, [&](Entity& e) {
            ExtData& x = e.common.ext.edit();
            std::vector<DictRecord> resolved;
            for (DictRecord& d : x.dict) {
                if (d.name != kPendingDict) {
                    resolved.push_back(std::move(d));
                    continue;
                }
                const auto dict = dicts_.find(d.items.empty() ? std::string() : d.items[0].text);
                if (dict == dicts_.end())
                    continue;
                for (const auto& [key, handle] : dict->second) {
                    const auto rec = xrecItems_.find(handle);
                    if (rec == xrecItems_.end() || resolved.size() >= 256)
                        continue;
                    resolved.push_back(DictRecord{key, rec->second});
                }
            }
            x.dict = std::move(resolved);
            e.common.ext.trim();
        });
    }
}

EntityCommon Importer::commonOf(const Record& r) {
    EntityCommon c;
    c.layer = layerFor(r.raw(8), r.line);
    c.level = levelFor(r);
    if (r.has(6))
        c.linetype = linetypeFor(r.raw(6), r.line);
    if (const Group* g = r.find(62)) {
        std::int64_t v = g->integer();
        if (v == 256)
            c.color = Color::byLayer();
        else if (v == 0)
            c.color = Color::byBlock();
        else
            c.color = mapAci(static_cast<int>(std::min<std::int64_t>(std::abs(v), 255)));
    }
    if (const Group* g = r.find(420)) {
        std::int64_t v = g->integer();
        c.color = Color::rgb(static_cast<std::uint8_t>((v >> 16) & 0xFF),
                             static_cast<std::uint8_t>((v >> 8) & 0xFF),
                             static_cast<std::uint8_t>(v & 0xFF));
    }
    if (const Group* g = r.find(370)) {
        std::int64_t v = g->integer();
        if (v >= 0)
            c.lineweight =
                LineWeight::hundredths(static_cast<std::int16_t>(std::min<std::int64_t>(v, 2000)));
        else if (v == -2)
            c.lineweight = LineWeight::byBlock();
        else if (v == -3)
            c.lineweight = LineWeight::defaultWeight();
        else
            c.lineweight = LineWeight::byLayer();
    }
    if (const Group* g = r.find(440))
        c.transparency = static_cast<std::int8_t>(transparencyFromGroup(g->integer()));
    if (const Group* g = r.find(48)) {
        double s = g->number();
        if (std::isfinite(s) && s > 0)
            c.linetypeScale = static_cast<float>(s);
    }
    c.visible = r.integer(60, 0) == 0;
    c.ext = ExtDataPtr(extOf(r));
    bool ours = false;
    for (const Group& g : r.g) {
        if (g.code == 1001)
            ours = g.str() == "TBCAD";
        else if (ours && g.code == 1000 && g.str() == "annotative=1")
            c.annotative = true;
        else if (ours && g.code == 1000 && g.str() == "wipeout=1")
            c.wipeout = true;
        else if (ours && g.code == 1000 && g.str().rfind("annoscales=", 0) == 0) {
            std::string list(g.str().substr(11));
            std::vector<double> found;
            std::size_t at = 0;
            while (at <= list.size() && found.size() < 64) {
                std::size_t comma = list.find(',', at);
                std::string part = list.substr(at, comma == std::string::npos ? comma : comma - at);
                char* end = nullptr;
                double v = std::strtod(part.c_str(), &end);
                if (end != part.c_str() && std::isfinite(v) && v > 0)
                    found.push_back(v);
                if (comma == std::string::npos)
                    break;
                at = comma + 1;
            }
            c.annoScales = ScaleList(std::move(found));
        }
    }
    return c;
}

bool Importer::frameOf(const Record& r, Frame& f, std::string_view type) {
    double nx = r.num(210, 0), ny = r.num(220, 0), nz = r.num(230, 1);
    double len = std::sqrt(nx * nx + ny * ny + nz * nz);
    if (!(len > 0) || !std::isfinite(len))
        return true;  // unusable extrusion: treat as the default
    nx /= len;
    ny /= len;
    nz /= len;
    // A tilt below about 3 degrees (real drawings carry such noise) is read as flat.
    if (std::abs(nx) < 5e-2 && std::abs(ny) < 5e-2) {
        f.mirror = nz < 0;
        return true;
    }
    // Up to about 25 degrees the object is drawn as seen from above: its coordinate axes follow
    // the arbitrary axis algorithm and the tilt is dropped.
    if (std::abs(nz) >= 0.9) {
        double ax = -ny, ay = nx;  // Wz x N, or Wy x N for a vertical-ish normal (not reached here)
        const double al = std::hypot(ax, ay);
        ax /= al;
        ay /= al;
        // Y axis = N x X axis, projected on the XY plane.
        const double bx = ny * 0 - nz * ay;
        const double by = nz * ax - nx * 0;
        const double det = ax * by - ay * bx;
        f.mirror = det < 0;
        f.rot = f.mirror ? std::atan2(-ay, -ax) : std::atan2(ay, ax);
        if (flattened_++ == 0)
            firstFlattenedLine_ = r.line;
        return true;
    }
    report_.warn(code::kExtrusion3d,
                 std::string(type) + " lies in a tilted 3D plane (extrusion " + formatDouble(nx) +
                     ", " + formatDouble(ny) + ", " + formatDouble(nz) + "); skipped",
                 r.line, handleOf(r));
    return false;
}

void Importer::note3d(const Record& r) {
    for (const Group& g : r.g) {
        if (g.code >= 30 && g.code <= 33 && std::abs(g.number()) > 1e-9) {
            if (flattened_++ == 0)
                firstFlattenedLine_ = r.line;
            return;
        }
    }
}

// ---------------------------------------------------------------------------------------------
// Dispatch

void Importer::add(Entity&& e, BlockId owner, std::string_view type, int line) {
    EntityCount& cnt = sum_.entities[std::string(type)];
    tbcad::Result<EntityId> res = tx_->add(std::move(e), owner);
    if (res) {
        ++cnt.imported;
        return;
    }
    ++cnt.skipped;
    if (res.error().code == ErrorCode::OutOfRange) {
        ++sum_.beyondHardCap;
        report_.warn(code::kBeyondHardCap,
                     std::string(type) + ": coordinates beyond the hard limit of 1e10 mm; dropped",
                     line);
    } else {
        report_.warn(code::kRejected,
                     std::string(type) + " rejected by the document: " + res.error().message, line);
    }
}

static std::string upperAscii(std::string s) {
    for (char& c : s)
        if (c >= 'a' && c <= 'z')
            c = static_cast<char>(c - 'a' + 'A');
    return s;
}

void Importer::handleEntity(Record& r, RecordReader& rr, BlockId owner) {
    std::string_view t = r.type;
    if (t == "SEQEND" || t == "VERTEX" || t == "ENDBLK" || t == "BLOCK")
        return;
    std::string type(t);
    // Paper space: the objects of a layout sheet are read in paper millimetres into the sheet's
    // own block (the unit scale of the model does not apply to them).
    struct PaperScale {
        double& k;
        double saved;
        PaperScale(double& kk, bool on) : k(kk), saved(kk) {
            if (on)
                k = 1;
        }
        ~PaperScale() { k = saved; }
    };
    std::size_t paperSheet = sheets_.size();
    if (sheetBlock_ && curSheet_ >= 0) {
        paperSheet = static_cast<std::size_t>(curSheet_);
    } else if (r.integer(67) == 1) {
        paperSheet = sheetFor("*paper_space");
    }
    const bool onSheet = paperSheet < sheets_.size() && sheets_[paperSheet].content;
    if (type == "VIEWPORT") {
        if (onSheet)
            readViewport(r, sheets_[paperSheet].content);
        return;
    }
    if (onSheet)
        owner = sheets_[paperSheet].content;
    else if (r.integer(67) == 1) {
        ++paperSpace_;
        return;
    }
    PaperScale paperScale(k_, onSheet);
    EntityCount& cnt = sum_.entities[type];
    ++cnt.read;
    note3d(r);

    std::vector<Entity> out;
    if (!convert(r, rr, out, owner)) {
        ++cnt.skipped;
        auto& s = skippedTypes_[type];
        if (s.first++ == 0)
            s.second = r.line;
        return;
    }
    if (out.empty()) {
        ++cnt.skipped;
        return;
    }
    // A closed polyline on a sheet may be the outline of a clipped viewport (group 340 of the
    // VIEWPORT names its handle).
    if (onSheet && t == "LWPOLYLINE" && out.size() == 1) {
        if (const auto* pl = entityAs<PolylineData>(out[0]);
            pl && pl->closed && pl->v.size() >= 3) {
            std::vector<Vec2> ring;
            for (const PolyVertex& v : pl->v)
                ring.push_back(v.p);
            sheetRings_[std::string(r.raw(5))] = std::move(ring);
        }
    }
    EntityCommon c = commonOf(r);
    for (Entity& e : out) {
        if (!e.common.layer) {
            const bool wipeout = e.common.wipeout;
            e.common = c;
            if (wipeout)
                e.common.wipeout = true;
        }
        add(std::move(e), owner, type, r.line);
    }
}

bool Importer::convert(Record& r, RecordReader& rr, std::vector<Entity>& out, BlockId owner) {
    std::string_view t = r.type;
    Frame f;
    auto withFrame = [&](auto&& fn) {
        if (frameOf(r, f, t))
            fn();
    };
    if (t == "LINE") {
        convLine(r, out);
    } else if (t == "CIRCLE") {
        withFrame([&] { convCircle(r, f, out); });
    } else if (t == "ARC") {
        withFrame([&] { convArc(r, f, out); });
    } else if (t == "LWPOLYLINE") {
        withFrame([&] { convLwPolyline(r, f, out); });
    } else if (t == "POLYLINE") {
        // The vertices are separate records that must be consumed even when the polyline is
        // skipped, so read them first.
        convPolyline(r, rr, f, out);
    } else if (t == "ELLIPSE") {
        withFrame([&] { convEllipse(r, f, out); });
    } else if (t == "SPLINE") {
        convSpline(r, out);
    } else if (t == "TEXT" || t == "ATTRIB" || t == "ATTDEF") {
        const TextKind kind = t == "TEXT"     ? TextKind::Text
                              : t == "ATTRIB" ? TextKind::Attrib
                                              : TextKind::AttDef;
        withFrame([&] { convText(r, f, kind, out); });
    } else if (t == "MTEXT") {
        withFrame([&] { convMText(r, f, out); });
    } else if (t == "POINT") {
        convPoint(r, out);
    } else if (t == "RAY" || t == "XLINE") {
        convRay(r, t == "XLINE", out);
    } else if (t == "INSERT") {
        withFrame([&] { convInsert(r, rr, f, out); });
    } else if (t == "HATCH") {
        withFrame([&] { convHatch(r, f, out); });
    } else if (t == "SOLID" || t == "TRACE") {
        withFrame([&] { convSolid(r, f, out); });
    } else if (t == "DIMENSION") {
        convDimension(r, out, owner);
    } else if (t == "LEADER") {
        convLeader(r, out);
    } else if (t == "MULTILEADER" || t == "MLEADER") {
        if (!convMLeader(r, out) && !convForeign(r, out))
            return false;
    } else if (t == "3DFACE") {
        conv3dFace(r, out);
    } else if (t == "MLINE") {
        convMLine(r, out);
    } else if (t == "WIPEOUT") {
        convWipeout(r, out);
    } else if (t == "IMAGE") {
        convImage(r, out);
    } else if (t == "TOLERANCE") {
        convTolerance(r, out);
    } else if (t == "ARC_DIMENSION") {
        convArcDimension(r, out);
    } else if (t == "ACAD_PROXY_ENTITY") {
        convProxy(r, out);
    } else if (!convForeign(r, out)) {
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------------------------
// Simple geometry

bool Importer::convLine(const Record& r, std::vector<Entity>& out) {
    LineData d{P(r, 10, 20), P(r, 11, 21)};
    out.push_back(makeEntity(d));
    return true;
}

bool Importer::convRay(const Record& r, bool infinite, std::vector<Entity>& out) {
    RayData d;
    d.origin = P(r, 10, 20);
    d.dir = P(r, 11, 21);
    d.infinite = infinite;
    const double len = std::hypot(d.dir.x, d.dir.y);
    if (!(len > 1e-12) || !std::isfinite(len)) {
        bad(r, infinite ? "XLINE" : "RAY", "direction must not be zero; skipped");
        return false;
    }
    d.dir = {d.dir.x / len, d.dir.y / len};
    out.push_back(makeEntity(d));
    return true;
}

bool Importer::convPoint(const Record& r, std::vector<Entity>& out) {
    out.push_back(makeEntity(PointData{P(r, 10, 20)}));
    return true;
}

bool Importer::convCircle(const Record& r, const Frame& f, std::vector<Entity>& out) {
    double radius = r.num(40) * k_;
    if (!(radius > 0) || !std::isfinite(radius)) {
        bad(r, "CIRCLE", "radius must be positive; skipped");
        return false;
    }
    out.push_back(makeEntity(CircleData{f.p(P(r, 10, 20)), radius}));
    return true;
}

bool Importer::convArc(const Record& r, const Frame& f, std::vector<Entity>& out) {
    double radius = r.num(40) * k_;
    if (!(radius > 0) || !std::isfinite(radius)) {
        bad(r, "ARC", "radius must be positive; skipped");
        return false;
    }
    double sa = r.num(50) * kDeg, ea = r.num(51) * kDeg;
    Vec2 c = f.p(P(r, 10, 20));
    if (tol::nearlyEqualAngle(sa, ea)) {
        if (std::abs(r.num(51) - r.num(50)) > 1.0) {
            out.push_back(makeEntity(CircleData{c, radius}));
            return true;
        }
        bad(r, "ARC", "start and end angle are equal; skipped");
        return false;
    }
    // Mirroring reverses the direction of travel, so start and end swap.
    double s = normalizeAngle(f.angle(f.mirror ? ea : sa));
    double e = normalizeAngle(f.angle(f.mirror ? sa : ea));
    out.push_back(makeEntity(ArcData{c, radius, s, e}));
    return true;
}

bool Importer::convLwPolyline(const Record& r, const Frame& f, std::vector<Entity>& out) {
    PolylineData pl;
    for (const Group& g : r.g) {
        switch (g.code) {
            case 10: {
                PolyVertex v;
                v.p.x = g.number() * k_;
                pl.v.push_back(v);
                break;
            }
            case 20:
                if (!pl.v.empty())
                    pl.v.back().p.y = g.number() * k_;
                break;
            case 40:
                if (!pl.v.empty())
                    pl.v.back().startWidth = g.number() * k_;
                break;
            case 41:
                if (!pl.v.empty())
                    pl.v.back().endWidth = g.number() * k_;
                break;
            case 42:
                if (!pl.v.empty())
                    pl.v.back().bulge = f.bulge(g.number());
                break;
            default:
                break;
        }
    }
    for (PolyVertex& v : pl.v)
        v.p = f.p(v.p);
    pl.closed = (r.integer(70) & 1) != 0;
    pl.constantWidth = std::max(0.0, r.num(43) * k_);
    if (pl.v.size() < 2) {
        bad(r, "LWPOLYLINE", "fewer than two vertices; skipped");
        return false;
    }
    out.push_back(makeEntity(std::move(pl)));
    return true;
}

bool Importer::convPolyline(const Record& r, RecordReader& rr, const Frame& /*unused*/,
                            std::vector<Entity>& out) {
    const std::int64_t flags = r.integer(70);
    const bool threeD = (flags & (8 | 16 | 64)) != 0;  // 3D polyline, mesh, polyface mesh
    const double defStart = r.num(40) * k_, defEnd = r.num(41) * k_;

    std::vector<PolyVertex> plain, splineVerts;
    Record v;
    while (rr.next(v)) {
        if (v.type == "VERTEX") {
            std::int64_t vf = v.integer(70);
            if (vf & 16)
                continue;  // spline frame control point
            PolyVertex pv;
            pv.p = P(v, 10, 20);
            pv.bulge = v.num(42);
            pv.startWidth = v.has(40) ? v.num(40) * k_ : defStart;
            pv.endWidth = v.has(41) ? v.num(41) * k_ : defEnd;
            (vf & 8 ? splineVerts : plain).push_back(pv);
        } else if (v.type == "SEQEND") {
            break;
        } else {
            rr.pushBack(std::move(v));
            break;
        }
    }
    if (threeD) {
        bad(r, "POLYLINE", "3D polylines and meshes are not supported; skipped");
        return false;
    }
    Frame f;
    if (!frameOf(r, f, "POLYLINE"))
        return false;
    PolylineData pl;
    pl.v = ((flags & (2 | 4)) && !splineVerts.empty()) ? std::move(splineVerts) : std::move(plain);
    for (PolyVertex& pv : pl.v) {
        pv.p = f.p(pv.p);
        pv.bulge = f.bulge(pv.bulge);
    }
    pl.closed = (flags & 1) != 0;
    if (pl.v.size() < 2) {
        bad(r, "POLYLINE", "fewer than two vertices; skipped");
        return false;
    }
    out.push_back(makeEntity(std::move(pl)));
    return true;
}

bool Importer::convEllipse(const Record& r, const Frame& f, std::vector<Entity>& out) {
    EllipseData d;
    d.center = P(r, 10, 20);
    d.majorAxis = P(r, 11, 21);
    double ratio = r.num(40, 1);
    double s = r.num(41, 0), e = r.num(42, kTwoPi);
    if (!(length(d.majorAxis) > 0) || !std::isfinite(ratio) || !(ratio > 0)) {
        bad(r, "ELLIPSE", "degenerate axes; skipped");
        return false;
    }
    if (ratio > 1) {
        // The "minor" axis is the longer one: swap the roles of the axes.
        d.majorAxis = perp(d.majorAxis) * ratio;
        ratio = 1 / ratio;
        s -= kPi / 2;
        e -= kPi / 2;
    }
    d.ratio = ratio;
    if (f.mirror) {
        // Parameters run counter-clockwise about the extrusion, i.e. clockwise seen from +Z.
        double ns = -e, ne = -s;
        s = ns;
        e = ne;
    }
    double sweep = e - s;
    if (!(sweep > 0))
        sweep = kTwoPi + std::fmod(sweep, kTwoPi);
    if (sweep >= kTwoPi - 1e-9) {
        d.startParam = 0;
        d.endParam = kTwoPi;
    } else {
        d.startParam = normalizeAngle(s);
        d.endParam = d.startParam + sweep;
    }
    out.push_back(makeEntity(std::move(d)));
    return true;
}

bool Importer::convSpline(const Record& r, std::vector<Entity>& out) {
    SplineData sp;
    sp.degree = static_cast<int>(r.integer(71, 3));
    std::int64_t flags = r.integer(70);
    sp.closed = (flags & 1) != 0;
    sp.periodic = (flags & 2) != 0;
    std::vector<Vec2> fit;
    std::optional<Vec2> t0, t1;
    for (const Group& g : r.g) {
        switch (g.code) {
            case 40:
                sp.knots.push_back(g.number());
                break;
            case 41:
                sp.weights.push_back(g.number());
                break;
            case 10:
                sp.control.push_back({g.number() * k_, 0});
                break;
            case 20:
                if (!sp.control.empty())
                    sp.control.back().y = g.number() * k_;
                break;
            case 11:
                fit.push_back({g.number() * k_, 0});
                break;
            case 21:
                if (!fit.empty())
                    fit.back().y = g.number() * k_;
                break;
            case 12:
                t0 = Vec2{g.number(), 0};
                break;
            case 22:
                if (t0)
                    t0->y = g.number();
                break;
            case 13:
                t1 = Vec2{g.number(), 0};
                break;
            case 23:
                if (t1)
                    t1->y = g.number();
                break;
            default:
                break;
        }
    }
    if (sp.degree < 1 || sp.degree > 15) {
        bad(r, "SPLINE", "unsupported degree " + std::to_string(sp.degree) + "; skipped");
        return false;
    }
    if (sp.control.empty()) {
        if (fit.size() < 2) {
            bad(r, "SPLINE", "no control or fit points; skipped");
            return false;
        }
        geom::InterpolateOptions opt;
        opt.degree = std::min(sp.degree, 3);
        opt.closed = sp.closed;
        opt.startTangent = t0;
        opt.endTangent = t1;
        tbcad::Result<SplineData> made = geom::interpolateSpline(fit, opt);
        if (!made) {
            bad(r, "SPLINE", "fit points cannot be interpolated (" + made.error().message + ")");
            return false;
        }
        out.push_back(makeEntity(std::move(made.value())));
        return true;
    }
    if (sp.control.size() < static_cast<std::size_t>(sp.degree) + 1) {
        bad(r, "SPLINE", "too few control points for its degree; skipped");
        return false;
    }
    if (!sp.knots.empty() &&
        (sp.knots.size() != sp.control.size() + static_cast<std::size_t>(sp.degree) + 1 ||
         !std::is_sorted(sp.knots.begin(), sp.knots.end()))) {
        bad(r, "SPLINE", "inconsistent knot vector replaced by a uniform one");
        sp.knots.clear();
    }
    if (!sp.weights.empty() && sp.weights.size() != sp.control.size()) {
        bad(r, "SPLINE", "weights do not match the control points and were dropped");
        sp.weights.clear();
    }
    sp.fit = std::move(fit);
    out.push_back(makeEntity(std::move(sp)));
    return true;
}

// ---------------------------------------------------------------------------------------------
// Text

bool Importer::convText(const Record& r, const Frame& f, TextKind kind, std::vector<Entity>& out) {
    const bool attrib = kind != TextKind::Text;  // ATTRIB and ATTDEF use group 74 for the baseline
    if (kind == TextKind::Attrib && (r.integer(70) & 1))
        return true;  // invisible attribute: nothing to show
    TextData d;
    std::string text = replacePercentCodes(dec(r.raw(1)), true);
    if (kind == TextKind::AttDef) {
        // The definition shows its tag in the block; the default value lives in group 1.
        AttDefInfo info;
        info.tag = upperAscii(dec(r.raw(2)));
        info.prompt = replacePercentCodes(dec(r.raw(3)));
        info.defaultValue = text;
        const std::int64_t flags = r.integer(70);
        info.invisible = (flags & 1) != 0;
        info.constant = (flags & 2) != 0;
        info.verify = (flags & 4) != 0;
        info.preset = (flags & 8) != 0;
        if (info.tag.empty() || info.tag.find_first_of(" \t\r\n") != std::string::npos) {
            bad(r, "ATTDEF", "attribute tag is empty or has spaces; imported as text");
            d.utf8 = text;
        } else {
            d.utf8 = info.tag;
            d.attdef = std::move(info);
        }
        text = d.utf8;
    }
    if (text.empty())
        return true;
    d.utf8 = std::move(text);
    d.position = f.p(P(r, 10, 20));
    d.height = r.num(40) * k_;
    if (!(d.height > 0) || !std::isfinite(d.height)) {
        d.height = 2.5;
        report_.info(code::kBadEntity, "TEXT: height is zero; using 2.5 mm", r.line);
    }
    d.rotation = f.angle(r.num(50) * kDeg);
    d.widthFactor = r.num(41, 1);
    if (!(d.widthFactor > 0) || !std::isfinite(d.widthFactor))
        d.widthFactor = 1;
    d.oblique = r.num(51) * kDeg;
    d.style = textStyleFor(r.raw(7));
    // In AutoCAD the style's width factor is only the default for new text: the object's own
    // factor is the one that applies, so it must not be multiplied by the style's again.
    if (auto it = styleRawWidth_.find(d.style.value); it != styleRawWidth_.end())
        d.widthFactor /= it->second;
    std::int64_t h = r.integer(72), v = r.integer(attrib ? 74 : 73);
    d.h = static_cast<HAlign>(std::clamp<std::int64_t>(h, 0, 5));
    d.v = static_cast<VAlign>(std::clamp<std::int64_t>(v, 0, 3));
    d.alignPoint = (h != 0 || v != 0) ? f.p(P(r, 11, 21)) : d.position;
    if (f.mirror) {
        if (mirroredText_++ == 0)
            firstMirroredLine_ = r.line;
    }
    out.push_back(makeEntity(std::move(d)));
    return true;
}

bool Importer::convMText(const Record& r, const Frame& f, std::vector<Entity>& out) {
    std::string raw;
    for (const Group& g : r.g) {
        if (g.code == 3 || g.code == 1)
            raw += g.str();
    }
    std::string text = convertMText(dec(raw), k_, mtextDropped_);
    if (text.empty())
        return true;
    MTextData d;
    d.utf8 = std::move(text);
    d.position = f.p(P(r, 10, 20));
    d.height = r.num(40) * k_;
    if (!(d.height > 0) || !std::isfinite(d.height)) {
        d.height = 2.5;
        report_.info(code::kBadEntity, "MTEXT: height is zero; using 2.5 mm", r.line);
    }
    d.width = std::max(0.0, r.num(41) * k_);
    double rot = r.has(11) ? std::atan2(r.num(21), r.num(11)) : r.num(50) * kDeg;
    d.rotation = f.angle(rot);
    d.attach = static_cast<Attachment>(std::clamp<std::int64_t>(r.integer(71, 1) - 1, 0, 8));
    d.style = textStyleFor(r.raw(7));
    if (f.mirror) {
        if (mirroredText_++ == 0)
            firstMirroredLine_ = r.line;
    }
    out.push_back(makeEntity(std::move(d)));
    return true;
}

// ---------------------------------------------------------------------------------------------
// Inserts, solids, leaders, proxies

bool Importer::blockHasAttDef(BlockId block, const std::string& tag) const {
    const BlockDef* def = doc_->block(block);
    if (!def)
        return false;
    for (EntityId id : def->entities)
        if (const Entity* e = doc_->find(id))
            if (const auto* t = entityAs<TextData>(*e); t && t->attdef && t->attdef->tag == tag)
                return !t->attdef->constant;
    return false;
}

bool Importer::convInsert(const Record& r, RecordReader& rr, const Frame& f,
                          std::vector<Entity>& out) {
    // Our own table export: an INSERT of an anonymous block with the table in extended data.
    {
        std::string packed;
        bool ours = false;
        for (const Group& g : r.g) {
            if (g.code == 1001) {
                ours = g.str() == "TBCAD";
            } else if (ours && g.code == 1000) {
                packed += dec(g.str());
            }
        }
        if (packed.rfind("tbcad-mline", 0) == 0) {
            if (auto m = mline::parse(packed)) {
                if (r.integer(66) == 1) {
                    Record skip;
                    while (rr.next(skip) && skip.type != "SEQEND") {
                    }
                }
                Entity e;
                e.data = Indirect<MlineData>(std::move(m->data));
                out.push_back(std::move(e));
                (void)tx_->modifyHeader([&](Header& h) { mline::storeStyle(h, m->style); });
                return true;
            }
        }
        if (packed.rfind("tbcad-table", 0) == 0) {
            if (auto t = table::parse(packed)) {
                if (r.integer(66) == 1) {  // no attributes expected, but keep the stream in step
                    Record skip;
                    while (rr.next(skip) && skip.type != "SEQEND") {
                    }
                }
                Entity e;
                e.data = Indirect<TableData>(std::move(*t));
                out.push_back(std::move(e));
                return true;
            }
        }
    }
    // An INSERT with group 66 = 1 is followed by its ATTRIB records up to SEQEND.
    std::vector<Record> attribs;
    if (r.integer(66) == 1) {
        for (;;) {
            Record a;
            if (!rr.next(a))
                break;
            if (a.type == "ATTRIB") {
                EntityCount& cnt = sum_.entities["ATTRIB"];
                ++cnt.read;
                ++cnt.imported;
                attribs.push_back(std::move(a));
            } else {
                if (a.type != "SEQEND")
                    rr.pushBack(std::move(a));
                break;
            }
        }
    }
    std::string_view rawName = r.raw(2);
    BlockId block = blockFor(rawName);
    if (!block) {
        bad(r, "INSERT", "block \"" + dec(rawName) + "\" cannot be referenced; skipped");
        return false;
    }
    double sx = r.num(41, 1), sy = r.num(42, 1);
    if (!(std::abs(sx) > 0) || !(std::abs(sy) > 0) || !std::isfinite(sx) || !std::isfinite(sy)) {
        bad(r, "INSERT", "zero scale; skipped");
        return false;
    }
    double rot = r.num(50) * kDeg;
    Vec2 pos = P(r, 10, 20);
    std::int64_t cols = std::max<std::int64_t>(1, r.integer(70, 1));
    std::int64_t rows = std::max<std::int64_t>(1, r.integer(71, 1));
    double cs = r.num(44) * k_, rs = r.num(45) * k_;
    if (cols > 100000 || rows > 100000 || cols * rows > 100000) {
        report_.warn(code::kInsertArray,
                     "INSERT array of " + std::to_string(cols) + " x " + std::to_string(rows) +
                         " is too large; only the first instance is imported",
                     r.line);
        cols = rows = 1;
    }
    // Values for the tags the block defines; any other ATTRIB stays visible as plain text.
    std::vector<AttribValue> values;
    std::vector<Entity> loose;
    for (const Record& a : attribs) {
        std::string tag = upperAscii(dec(a.raw(2)));
        std::string value = replacePercentCodes(dec(a.raw(1)), true);
        if (blockHasAttDef(block, tag)) {
            bool seen = false;
            for (const AttribValue& v : values)
                seen = seen || v.tag == tag;
            if (!seen)
                values.push_back({std::move(tag), std::move(value)});
            continue;
        }
        Frame af;
        if (!frameOf(a, af, "ATTRIB"))
            continue;
        std::vector<Entity> text;
        convText(a, af, TextKind::Attrib, text);
        for (Entity& e : text) {
            e.common = commonOf(a);
            loose.push_back(std::move(e));
        }
    }
    // TBCAD XDATA: clipping boundary in block coordinates (1000 "clip=<on>", then 1010/1020
    // points).
    std::vector<Vec2> clip;
    bool clipOn = true;
    {
        bool inOurs = false;
        Vec2 pending{};
        for (const Group& g : r.g) {
            if (g.code == 1001) {
                inOurs = g.str() == "TBCAD";
            } else if (!inOurs) {
                continue;
            } else if (g.code == 1000) {
                if (g.str() == "clip=0")
                    clipOn = false;
            } else if (g.code == 1010) {
                pending.x = g.number() * k_;
            } else if (g.code == 1020) {
                pending.y = g.number() * k_;
                clip.push_back(pending);
            }
        }
        if (clip.size() < 3)
            clip.clear();
    }
    double cr = std::cos(rot), sr = std::sin(rot);
    for (std::int64_t row = 0; row < rows; ++row) {
        for (std::int64_t col = 0; col < cols; ++col) {
            double ox = static_cast<double>(col) * cs, oy = static_cast<double>(row) * rs;
            Vec2 p = pos + Vec2{ox * cr - oy * sr, ox * sr + oy * cr};
            InsertData d;
            d.block = block;
            d.position = f.p(p);
            d.scale = f.mirror ? Vec2{-sx, sy} : Vec2{sx, sy};
            d.rotation = f.rot + (f.mirror ? -rot : rot);
            d.attribs = values;
            d.clip = clip;
            d.clipOn = clipOn;
            out.push_back(makeEntity(std::move(d)));
        }
    }
    for (Entity& e : loose)
        out.push_back(std::move(e));
    return true;
}

bool Importer::convSolid(const Record& r, const Frame& f, std::vector<Entity>& out) {
    Vec2 p1 = P(r, 10, 20), p2 = P(r, 11, 21), p3 = P(r, 12, 22);
    Vec2 p4 = r.has(13) ? P(r, 13, 23) : p3;
    HatchPolylineEdge edge;
    edge.closed = true;
    auto push = [&](Vec2 p) {
        PolyVertex v;
        v.p = f.p(p);
        edge.v.push_back(v);
    };
    push(p1);
    push(p2);
    if (tol::nearlyEqual(p3, p4)) {
        push(p3);
    } else {
        push(p4);  // DXF stores the corners in "bow-tie" order 1, 2, 4, 3
        push(p3);
    }
    HatchData h;
    h.solid = true;
    HatchLoop loop;
    loop.outer = true;
    loop.edges.emplace_back(std::move(edge));
    h.loops.push_back(std::move(loop));
    out.push_back(makeEntity(std::move(h)));
    return true;
}

bool Importer::convLeader(const Record& r, std::vector<Entity>& out) {
    LeaderData d;
    for (const Group& g : r.g) {
        if (g.code == 10)
            d.vertices.push_back({g.number() * k_, 0});
        else if (g.code == 20 && !d.vertices.empty())
            d.vertices.back().y = g.number() * k_;
    }
    if (d.vertices.size() < 2) {
        bad(r, "LEADER", "fewer than two vertices; skipped");
        return false;
    }
    d.style = dimStyleFor(r.raw(3));
    d.textPos = d.vertices.back();
    out.push_back(makeEntity(std::move(d)));
    return true;
}

// MLEADER: the leader lines, the dogleg and the text of a multileader, as one multileader. Block
// content and the leader style's look are not read.
bool Importer::convMLeader(const Record& r, std::vector<Entity>& out) {
    struct Branch {
        std::vector<Vec2> pts{};
        Vec2 last{};
        Vec2 dir{};
        double dogLen = 0;
        bool hasLast = false, hasDir = false;
    };
    std::vector<Branch> branches;
    std::string rawText;
    Vec2 base{}, textLoc{};
    bool hasBase = false, hasTextLoc = false;
    bool inCtx = false, inLeader = false, inLine = false;
    for (const Group& g : r.g) {
        const std::string_view s = g.code >= 300 && g.code < 320 ? g.str() : std::string_view{};
        if (g.code == 300 && s == "CONTEXT_DATA{") {
            inCtx = true;
        } else if (g.code == 301) {
            inCtx = false;
        } else if (g.code == 302 && s == "LEADER{") {
            inLeader = true;
            branches.emplace_back();
        } else if (g.code == 303) {
            inLeader = false;
        } else if (g.code == 304 && s == "LEADER_LINE{") {
            inLine = true;
        } else if (g.code == 305) {
            inLine = false;
        } else if (g.code == 304 && inCtx && !inLeader) {
            rawText += g.str();
        } else if (inLine && !branches.empty()) {
            if (g.code == 10)
                branches.back().pts.push_back({g.number() * k_, 0});
            else if (g.code == 20 && !branches.back().pts.empty())
                branches.back().pts.back().y = g.number() * k_;
        } else if (inLeader && !branches.empty()) {
            Branch& b = branches.back();
            if (g.code == 10) {
                b.last.x = g.number() * k_;
                b.hasLast = true;
            } else if (g.code == 20) {
                b.last.y = g.number() * k_;
            } else if (g.code == 11) {
                b.dir.x = g.number();
                b.hasDir = true;
            } else if (g.code == 21) {
                b.dir.y = g.number();
            } else if (g.code == 40) {
                b.dogLen = g.number() * k_;
            }
        } else if (inCtx) {
            if (g.code == 10) {
                base.x = g.number() * k_;
                hasBase = true;
            } else if (g.code == 20) {
                base.y = g.number() * k_;
            } else if (g.code == 12) {
                textLoc.x = g.number() * k_;
                hasTextLoc = true;
            } else if (g.code == 22) {
                textLoc.y = g.number() * k_;
            }
        }
    }
    const std::string text = convertMText(dec(rawText), k_, mtextDropped_);
    const Vec2 anchor = hasTextLoc ? textLoc : base;
    const DimStyleId style = dimStyleFor(r.raw(3));
    MLeaderData m;
    m.style = style;
    bool haveLanding = false, haveDog = false;
    for (Branch& b : branches) {
        if (b.pts.empty())
            continue;
        // The vertices run from one end to the other: the end nearer the text is the landing.
        const Vec2 near = b.hasLast ? b.last : anchor;
        if (distance(b.pts.front(), near) < distance(b.pts.back(), near))
            std::reverse(b.pts.begin(), b.pts.end());
        std::vector<Vec2> line = b.pts;  // arrowhead first
        if (b.hasLast && distance(line.back(), b.last) > tol::kLinear)
            line.push_back(b.last);
        if (line.size() < 2)
            continue;
        if (!haveLanding) {
            m.landing = line.back();
            haveLanding = true;
        }
        if (!haveDog && b.hasDir && b.dogLen > 0 && !tol::isZeroLength(b.dir)) {
            m.contentPos = m.landing + normalized(b.dir) * b.dogLen;
            haveDog = true;
        }
        m.lines.push_back(std::move(line));
    }
    if (!m.lines.empty()) {
        if (!haveDog)
            m.contentPos = hasTextLoc || hasBase ? anchor : m.landing;
        m.dogleg = distance(m.landing, m.contentPos) > tol::kLinear;
        if (text.empty())
            m.content = MLeaderData::Content::None;
        m.utf8 = text;
        out.push_back(makeEntity(std::move(m)));
    } else if (!text.empty()) {
        MTextData mt;
        mt.utf8 = text;
        mt.position = anchor;
        mt.height = 2.5;
        out.push_back(makeEntity(std::move(mt)));
    }
    return !out.empty();
}

// 3DFACE: the visible edges of the face projected on the XY plane.
bool Importer::conv3dFace(const Record& r, std::vector<Entity>& out) {
    Vec2 p[4] = {P(r, 10, 20), P(r, 11, 21), P(r, 12, 22), r.has(13) ? P(r, 13, 23) : P(r, 12, 22)};
    const std::int64_t hidden = r.integer(70);
    const int n = tol::nearlyEqual(p[2], p[3]) ? 3 : 4;
    for (int i = 0; i < n; ++i) {
        if (hidden & (std::int64_t{1} << i))
            continue;
        const Vec2 a = p[i], b = p[(i + 1) % n];
        if (distance(a, b) > tol::kLinear)
            out.push_back(makeEntity(LineData{a, b}));
    }
    return !out.empty();
}

// MLINE: one polyline per element of the style. Each vertex carries its position, the miter
// direction and, for every element, the distance along that direction at which the element
// passes (the first number of the element's parameter list). The style table is not needed.
bool Importer::convMLine(const Record& r, std::vector<Entity>& out) {
    struct Vertex {
        Vec2 pos{}, miter{};
        std::vector<double> offset{};
    };
    std::vector<Vertex> vs;
    bool first = false;  // the next 41 is the first number of an element's list
    for (const Group& g : r.g) {
        if (g.code == 11) {
            vs.emplace_back();
            vs.back().pos.x = g.number() * k_;
        } else if (vs.empty()) {
            continue;
        } else if (g.code == 21) {
            vs.back().pos.y = g.number() * k_;
        } else if (g.code == 13) {
            vs.back().miter.x = g.number();
        } else if (g.code == 23) {
            vs.back().miter.y = g.number();
        } else if (g.code == 74) {
            first = true;
        } else if (g.code == 41 && first) {
            vs.back().offset.push_back(g.number() * k_);
            first = false;
        }
    }
    std::size_t elements = vs.empty() ? 0 : vs.front().offset.size();
    for (const Vertex& v : vs)
        elements = std::min(elements, v.offset.size());
    if (vs.size() < 2 || elements == 0) {
        return convForeign(r, out);
    }
    const bool closed = (r.integer(71) & 2) != 0;
    for (std::size_t e = 0; e < elements; ++e) {
        PolylineData pl;
        pl.closed = closed && vs.size() > 2;
        for (const Vertex& v : vs) {
            PolyVertex pv;
            pv.p = v.pos + v.miter * v.offset[e];
            pl.v.push_back(pv);
        }
        out.push_back(makeEntity(std::move(pl)));
    }
    return true;
}

// WIPEOUT: a closed polyline that hides what is behind it. The clip boundary is in pixel units of
// an image frame: world = insert + u * (x + 0.5) + v * (y + 0.5).
bool Importer::convWipeout(const Record& r, std::vector<Entity>& out) {
    const Vec2 origin = P(r, 10, 20), u = P(r, 11, 21), v = P(r, 12, 22);
    std::vector<Vec2> pix;
    for (const Group& g : r.g) {
        if (g.code == 14)
            pix.push_back({g.number(), 0});
        else if (g.code == 24 && !pix.empty())
            pix.back().y = g.number();
    }
    if (r.integer(71) == 1 && pix.size() == 2) {  // rectangle: two opposite corners
        const Vec2 a = pix[0], b = pix[1];
        pix = {a, {b.x, a.y}, b, {a.x, b.y}};
    }
    if (pix.size() > 1 && tol::nearlyEqual(pix.front(), pix.back()))
        pix.pop_back();
    if (pix.size() < 3) {
        bad(r, "WIPEOUT", "fewer than three boundary points; skipped");
        return false;
    }
    PolylineData pl;
    pl.closed = true;
    for (const Vec2& p : pix) {
        PolyVertex vtx;
        vtx.p = origin + u * (p.x + 0.5) + v * (p.y + 0.5);
        pl.v.push_back(vtx);
    }
    Entity e = makeEntity(std::move(pl));
    e.common.wipeout = true;
    out.push_back(std::move(e));
    return true;
}

// IMAGE: a raster picture. It becomes the same thing IMAGEATTACH makes: a block that is an external
// reference to the picture file (a frame in pixel units, origin bottom-left) inserted with the size
// of one pixel as its scale. The file name is in the IMAGEDEF object, read later (resolveImages).
// A rectangular clip and the fade are kept; a polygonal clip is not (the whole picture shows).
bool Importer::convImage(const Record& r, std::vector<Entity>& out) {
    const Vec2 origin = P(r, 10, 20), u = P(r, 11, 21);
    const double w = r.num(13), h = r.num(23);
    const double px = std::hypot(u.x, u.y);
    if (!(w >= 1) || !(h >= 1) || !(px > 0) || !std::isfinite(px)) {
        return convForeign(r, out);
    }
    std::string handle(r.raw(340));
    std::string base = "IMAGE_" + (handle.empty() ? std::string("0") : handle);
    const int n = imageNames_[base]++;
    const std::string name = n == 0 ? base : base + "_" + std::to_string(n + 1);
    const BlockId block = blockFor(name);
    if (!block)
        return false;
    blockDefined_.insert(block);
    ++sum_.blocks;

    PolylineData frame;
    frame.closed = true;
    for (const Vec2 p : {Vec2{0, 0}, Vec2{w, 0}, Vec2{w, h}, Vec2{0, h}})
        frame.v.push_back({p, 0, 0, 0});
    Entity fe = makeEntity(std::move(frame));
    fe.common = commonOf(r);
    (void)tx_->add(std::move(fe), block);

    PendingImage img;
    img.block = block;
    img.name = name;
    img.defHandle = handle;
    img.line = r.line;
    img.fade = std::clamp(r.num(283), 0.0, underlay::kMaxImageFade);
    std::vector<Vec2> clip;
    for (const Group& g : r.g) {
        if (g.code == 14)
            clip.push_back({g.number(), 0});
        else if (g.code == 24 && !clip.empty())
            clip.back().y = g.number();
    }
    if (r.integer(71) == 2 && clip.size() >= 3) {
        // A polygon: use its bounding rectangle when it is one, else leave the picture whole.
        if (clip.size() > 1 && tol::nearlyEqual(clip.front(), clip.back()))
            clip.pop_back();
        const bool box = clip.size() == 4 && ((clip[0].x == clip[1].x) != (clip[0].y == clip[1].y));
        if (!box) {
            report_.warn(code::kRejected,
                         "IMAGE \"" + name +
                             "\": a polygon clip is not supported; the whole picture "
                             "is shown",
                         r.line);
            clip.clear();
        }
    }
    if (clip.size() >= 2) {
        double x0 = clip[0].x, x1 = clip[0].x, y0 = clip[0].y, y1 = clip[0].y;
        for (const Vec2& c : clip) {
            x0 = std::min(x0, c.x);
            x1 = std::max(x1, c.x);
            y0 = std::min(y0, c.y);
            y1 = std::max(y1, c.y);
        }
        img.u0 = std::clamp((x0 + 0.5) / w, 0.0, 1.0);
        img.u1 = std::clamp((x1 + 0.5) / w, 0.0, 1.0);
        img.v0 = std::clamp(1.0 - (y1 + 0.5) / h, 0.0, 1.0);
        img.v1 = std::clamp(1.0 - (y0 + 0.5) / h, 0.0, 1.0);
        if (!(img.u1 > img.u0) || !(img.v1 > img.v0)) {
            img.u0 = img.v0 = 0;
            img.u1 = img.v1 = 1;
        }
    }
    images_.push_back(std::move(img));

    InsertData d;
    d.block = block;
    d.position = origin;
    d.scale = {px, px};
    d.rotation = std::atan2(u.y, u.x);
    out.push_back(makeEntity(std::move(d)));
    return true;
}

// A geometric tolerance (feature control frame) becomes a block insert drawn at the size of the
// dimension style it names, like the TOLERANCE command makes; the table in the header remembers
// what it says so that it can be written back as a TOLERANCE.
bool Importer::convTolerance(const Record& r, std::vector<Entity>& out) {
    const auto spec = gdt::parseDxfText(dec(r.raw(1)));
    if (!spec)
        return convForeign(r, out);
    std::string name;
    do
        name = "TB_TOL_" + std::to_string(++toleranceBlocks_);
    while (doc_->blockByName(name));
    const BlockId block = blockFor(name);
    if (!block)
        return false;
    blockDefined_.insert(block);
    ++sum_.blocks;
    for (Entity& e : gdt::frameEntities(*spec)) {
        e.common.layer = kLayerZero;
        (void)tx_->add(std::move(e), block);
    }
    (void)gdt::addFrame(*tx_, name, *spec);

    // The frame is 2.5 mm text on paper; the dimension style says how big it is here.
    double size = gdt::kTextHeight;
    if (const DimStyle* ds = doc_->dimStyleByName(dec(r.raw(3))))
        size = ds->textHeight * (ds->overallScale > 0 ? ds->overallScale : 1.0);
    const double n = std::isfinite(size) && size > 0 ? size / gdt::kTextHeight : 1.0;
    const Vec2 dir = P(r, 11, 21);
    InsertData d;
    d.block = block;
    d.position = P(r, 10, 20);
    d.scale = {n, n};
    d.rotation = (std::hypot(dir.x, dir.y) > 1e-12) ? std::atan2(dir.y, dir.x) : 0.0;
    out.push_back(makeEntity(std::move(d)));
    return true;
}

// Gives the picture blocks their file (IMAGEDEF) and display settings (fade, clip).
void Importer::resolveImages() {
    underlay::ImageParamTable table = underlay::readImageParams(doc_->header());
    for (const PendingImage& img : images_) {
        auto it = imageDefs_.find(img.defHandle);
        if (it == imageDefs_.end() || it->second.empty()) {
            report_.warn(code::kXref, "IMAGE \"" + img.name + "\" has no image file (IMAGEDEF)",
                         img.line);
            continue;
        }
        std::string path = it->second;
        std::replace(path.begin(), path.end(), '\\', '/');
        (void)tx_->modifyBlock(img.block, [&](BlockDef& b) { b.xrefPath = path; });
        underlay::ImageParams p;
        p.fade = img.fade;
        p.u0 = img.u0;
        p.v0 = img.v0;
        p.u1 = img.u1;
        p.v1 = img.v1;
        if (!(p == underlay::ImageParams{}))
            table[img.name] = p;
        if (!underlay::isImagePath(path))
            report_.warn(code::kXref,
                         "IMAGE \"" + img.name + "\": \"" + path +
                             "\" is not a PNG, JPEG or BMP file; it shows only as a frame",
                         img.line);
    }
    if (!images_.empty())
        (void)tx_->modifyHeader([&](Header& h) { underlay::writeImageParams(h, table); });
}

bool Importer::convProxy(const Record& r, std::vector<Entity>& out) {
    ProxyData d;
    d.originalType = "ACAD_PROXY_ENTITY";
    for (const Group& g : r.g) {
        if (g.code >= 310 && g.code <= 319) {
            if (const auto* bytes = std::get_if<std::vector<std::byte>>(&g.value)) {
                for (std::byte b : *bytes)
                    d.raw.push_back(static_cast<std::uint8_t>(b));
            }
        }
    }
    std::vector<Entity> texts;
    std::string problem;
    std::vector<std::uint8_t> gfx = proxyGraphicsOf(r);
    std::size_t unknown = 0;
    if (!gfx.empty())
        unknown = decodeProxyGraphics(gfx, commonOf(r), d, texts, problem);
    if (!texts.empty())
        stripProxyText(d.raw);
    const bool drawn = !d.graphics.items.empty() || !texts.empty();
    std::string cls(r.raw(2));
    report_.warn(code::kProxy,
                 drawn ? "ACAD_PROXY_ENTITY" + (cls.empty() ? "" : " (" + dec(cls) + ")") +
                             " is shown from its proxy graphics and kept read-only"
                       : "ACAD_PROXY_ENTITY kept as a read-only placeholder; it has no usable "
                         "proxy graphics, so it displays nothing",
                 r.line, handleOf(r));
    if (!problem.empty())
        report_.warn(code::kProxyDamaged, "ACAD_PROXY_ENTITY: " + problem, r.line, handleOf(r));
    if (unknown > 0) {
        report_.info(code::kProxyUnknown,
                     "ACAD_PROXY_ENTITY: " + std::to_string(unknown) +
                         " proxy graphics record(s) of an unknown or unusable kind were skipped",
                     r.line);
    }
    out.push_back(makeEntity(std::move(d)));
    for (Entity& t : texts)
        out.push_back(std::move(t));
    return true;
}

// An entity this importer has no reader for (AEC and Civil objects, 3DSOLID, REGION, SURFACE ...)
// is kept when it carries proxy graphics: they are drawn as a read-only proxy named after the
// DXF type. Without graphics it is skipped, as before.
bool Importer::convForeign(const Record& r, std::vector<Entity>& out) {
    std::vector<std::uint8_t> gfx = proxyGraphicsOf(r);
    if (gfx.size() < 16)
        return false;
    ProxyData d;
    d.originalType = std::string(r.type);
    std::vector<Entity> texts;
    std::string problem;
    std::size_t unknown = decodeProxyGraphics(gfx, commonOf(r), d, texts, problem);
    if (d.graphics.items.empty() && texts.empty())
        return false;
    std::string type(r.type);
    if (foreignShown_.insert(type).second) {
        report_.info(
            code::kProxyGraphics,
            type + ": no reader for this object; shown from its proxy graphics (read-only)",
            r.line);
    }
    if (!problem.empty())
        report_.warn(code::kProxyDamaged, type + ": " + problem, r.line, handleOf(r));
    if (unknown > 0) {
        report_.info(code::kProxyUnknown,
                     type + ": " + std::to_string(unknown) +
                         " proxy graphics record(s) of an unknown or unusable kind were skipped",
                     r.line);
    }
    out.push_back(makeEntity(std::move(d)));
    for (Entity& t : texts)
        out.push_back(std::move(t));
    return true;
}

}  // namespace tbcad::dxf::detail
