// DXF import: one record -> TBCAD entities (everything except HATCH and DIMENSION).
#include <algorithm>
#include <array>
#include <cmath>
#include <set>

#include "importer.h"
#include "keep.h"
#include "tbcad/gdt/gdt.h"
#include "tbcad/geom/spline.h"
#include "tbcad/mline/mline.h"
#include "tbcad/table/table.h"
#include "tbcad/underlay/image.h"
#include "tbcad/underlay/pdf.h"

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

// The scales an AutoCAD annotative object lists: its extension dictionary holds
// AcDbContextDataManager > ACDB_ANNOTATIONSCALES > one context-data object per scale.
ScaleList Importer::annotativeScalesOf(const std::vector<std::pair<std::string, std::string>>& xd) {
    auto child = [&](const std::vector<std::pair<std::string, std::string>>& entries,
                     const char* name) -> const std::vector<std::pair<std::string, std::string>>* {
        for (const auto& [key, handle] : entries)
            if (key == name) {
                const auto it = dicts_.find(handle);
                return it == dicts_.end() ? nullptr : &it->second;
            }
        return nullptr;
    };
    const auto* mgr = child(xd, "AcDbContextDataManager");
    const auto* scales = mgr ? child(*mgr, "ACDB_ANNOTATIONSCALES") : nullptr;
    std::vector<double> found;
    if (scales)
        for (const auto& entry : *scales) {
            const auto ctx = contextScale_.find(entry.second);
            if (ctx == contextScale_.end())
                continue;
            const auto sc = scaleFactors_.find(ctx->second);
            if (sc != scaleFactors_.end() && found.size() < 64 &&
                std::find(found.begin(), found.end(), sc->second) == found.end())
                found.push_back(sc->second);
        }
    return ScaleList(std::move(found));
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
                if (e.common.annotative && e.common.annoScales.empty())
                    e.common.annoScales = annotativeScalesOf(dict->second);
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
    {  // AutoCAD: AcadAnnotative XDATA "AnnotativeData", { 1070 1 (annotative) ... }
        bool inAnno = false, seenData = false;
        for (const Group& g : r.g) {
            if (g.code == 1001)
                inAnno = g.str() == "AcadAnnotative";
            else if (inAnno && g.code == 1000 && g.str() == "AnnotativeData")
                seenData = true;
            else if (inAnno && seenData && g.code == 1070) {
                c.annotative = g.integer() != 0;
                break;
            }
        }
    }
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

std::optional<Tilt> Importer::tiltOf(const Record& r) const {
    double nx = r.num(210, 0), ny = r.num(220, 0), nz = r.num(230, 1);
    const double len = std::sqrt(nx * nx + ny * ny + nz * nz);
    if (!(len > 0) || !std::isfinite(len))
        return std::nullopt;
    nx /= len;
    ny /= len;
    nz /= len;
    // frameOf reads anything closer to the XY plane than this as flat.
    if ((std::abs(nx) < 5e-2 && std::abs(ny) < 5e-2) || std::abs(nz) >= 0.9)
        return std::nullopt;
    Tilt t;
    t.n[0] = nx;
    t.n[1] = ny;
    t.n[2] = nz;
    // Arbitrary axis algorithm: X = Wz x N (N is far from the Z axis here), Y = N x X.
    const double al = std::hypot(nx, ny);
    t.ax[0] = -ny / al;
    t.ax[1] = nx / al;
    t.ax[2] = 0;
    t.ay[0] = ny * t.ax[2] - nz * t.ax[1];
    t.ay[1] = nz * t.ax[0] - nx * t.ax[2];
    t.ay[2] = nx * t.ax[1] - ny * t.ax[0];
    return t;
}

// The ellipse c + u cos t + v sin t (world coordinates, t from s to e) seen from above. Projected,
// u and v are conjugate half-diameters of an ellipse; its axes are found where the radius is
// longest. A plane seen edge-on leaves a straight piece, drawn as a polyline.
void Importer::addProjectedEllipse(const double c[3], const double u3[3], const double v3[3],
                                   double s, double e, bool full, std::vector<Entity>& out) {
    const Vec2 c2{c[0], c[1]}, u{u3[0], u3[1]}, v{v3[0], v3[1]};
    if (full) {
        s = 0;
        e = kTwoPi;
    }
    const double t0 = 0.5 * std::atan2(2 * dot(u, v), dot(u, u) - dot(v, v));
    const Vec2 a = u * std::cos(t0) + v * std::sin(t0);
    const Vec2 b = v * std::cos(t0) - u * std::sin(t0);
    const double la = length(a), lb = length(b);
    if (!(la > 1e-9) || !std::isfinite(la))
        return;
    if (lb < la * 1e-6) {
        PolylineData pl;
        const int n = 64;
        for (int i = 0; i <= n; ++i) {
            const double t = s + (e - s) * i / n;
            pl.v.push_back({c2 + u * std::cos(t) + v * std::sin(t), 0, 0, 0});
        }
        out.push_back(makeEntity(std::move(pl)));
        return;
    }
    EllipseData d;
    d.center = c2;
    d.majorAxis = a;
    d.ratio = lb / la;
    // b on the left of a: the parameter runs the same way; on the right it runs backwards.
    double ns = s - t0, ne = e - t0;
    if (a.x * b.y - a.y * b.x < 0) {
        ns = -(e - t0);
        ne = -(s - t0);
    }
    const double sweep = ne - ns;
    if (full || sweep >= kTwoPi - 1e-9) {
        d.startParam = 0;
        d.endParam = kTwoPi;
    } else {
        d.startParam = normalizeAngle(ns);
        d.endParam = d.startParam + sweep;
    }
    out.push_back(makeEntity(std::move(d)));
}

namespace {

// Points along an arc of a polyline segment from `p0` with `bulge`, both ends excluded.
void bulgePoints(Vec2 p0, Vec2 p1, double bulge, std::vector<Vec2>& out) {
    const Vec2 ch = p1 - p0;
    if (std::abs(bulge) < 1e-9 || !std::isfinite(bulge) || !(length(ch) > 0))
        return;
    const Vec2 centre = p0 + ch * 0.5 + perp(ch) * ((1 - bulge * bulge) / (4 * bulge));
    const double rad = length(p0 - centre);
    const double a0 = std::atan2(p0.y - centre.y, p0.x - centre.x);
    const double sweep = 4 * std::atan(bulge);
    const int k = std::clamp(static_cast<int>(std::ceil(std::abs(sweep) / (kPi / 18))), 2, 72);
    for (int j = 1; j < k; ++j) {
        const double a = a0 + sweep * j / k;
        out.push_back(centre + Vec2{std::cos(a), std::sin(a)} * rad);
    }
}

// Points along a counter-clockwise arc of an ellipse (`minor` is the second half axis) from
// parameter `from` to `to`, both ends included, reversed when `ccw` is false.
std::vector<Vec2> arcPoints(Vec2 c, Vec2 major, Vec2 minor, double from, double to, bool ccw) {
    if (!ccw)
        std::swap(from, to);
    double sweep = std::fmod(to - from, kTwoPi);
    if (sweep <= 1e-12)
        sweep += kTwoPi;
    const int k = std::clamp(static_cast<int>(std::ceil(sweep / (kPi / 36))), 2, 144);
    std::vector<Vec2> pts;
    for (int j = 0; j <= k; ++j) {
        const double a = from + sweep * j / k;
        pts.push_back(c + major * std::cos(a) + minor * std::sin(a));
    }
    if (!ccw)
        std::reverse(pts.begin(), pts.end());
    return pts;
}

}  // namespace

// An object-coordinate path with bulges, projected: the arcs become short segments.
std::vector<PolyVertex> Importer::projectPath(const std::vector<PolyVertex>& vs, bool closed,
                                              const Tilt& t, double elev) {
    std::vector<PolyVertex> out;
    const std::size_t n = vs.size(), segs = closed ? n : n - 1;
    std::vector<Vec2> mid;
    for (std::size_t i = 0; i < n; ++i) {
        out.push_back({t.map(vs[i].p, elev), 0, 0, 0});
        if (i >= segs)
            continue;
        mid.clear();
        bulgePoints(vs[i].p, vs[(i + 1) % n].p, vs[i].bulge, mid);
        for (Vec2 q : mid)
            out.push_back({t.map(q, elev), 0, 0, 0});
    }
    return out;
}

// A hatch built in object coordinates, projected. Arcs, elliptical arcs and bulges become line
// segments; splines keep their shape (projection is affine).
void Importer::projectHatch(HatchData& h, const Tilt& t, double elev) {
    auto m = [&](Vec2 p) { return t.map(p, elev); };
    auto lines = [&](const std::vector<Vec2>& pts, std::vector<HatchEdge>& out) {
        for (std::size_t i = 1; i < pts.size(); ++i)
            out.emplace_back(HatchLineEdge{m(pts[i - 1]), m(pts[i])});
    };
    for (HatchLoop& loop : h.loops) {
        std::vector<HatchEdge> edges;
        for (HatchEdge& e : loop.edges) {
            if (auto* l = std::get_if<HatchLineEdge>(&e)) {
                edges.emplace_back(HatchLineEdge{m(l->a), m(l->b)});
            } else if (auto* a = std::get_if<HatchArcEdge>(&e)) {
                lines(arcPoints(a->center, {a->radius, 0}, {0, a->radius}, a->startAngle,
                                a->endAngle, a->ccw),
                      edges);
            } else if (auto* el = std::get_if<HatchEllipseEdge>(&e)) {
                lines(arcPoints(el->center, el->majorAxis, perp(el->majorAxis) * el->ratio,
                                el->startParam, el->endParam, el->ccw),
                      edges);
            } else if (auto* sp = std::get_if<HatchSplineEdge>(&e)) {
                for (Vec2& p : sp->spline.control)
                    p = m(p);
                for (Vec2& p : sp->spline.fit)
                    p = m(p);
                edges.push_back(std::move(e));
            } else if (auto* pl = std::get_if<HatchPolylineEdge>(&e)) {
                if (pl->v.size() >= 2)
                    pl->v = projectPath(pl->v, pl->closed, t, elev);
                edges.push_back(std::move(e));
            }
        }
        loop.edges = std::move(edges);
    }
    h.patternOrigin = m(h.patternOrigin);
}

// A text built in object coordinates, projected. The projection of the text's plane is an affine
// map, which the rotation, height, width factor, obliquing angle and upside-down flag of a text
// can express exactly. False when the text is seen edge-on.
bool Importer::projectText(TextData& d, const Tilt& t, double elev) {
    auto lin = [&](Vec2 v) { return t.map(v, 0); };
    const Vec2 e1{std::cos(d.rotation), std::sin(d.rotation)};
    const Vec2 dir = lin(e1);
    const Vec2 up = lin(perp(e1) + e1 * std::tan(d.oblique));
    const double dl = length(dir);
    if (!(dl > 1e-6))
        return false;
    const Vec2 dh = dir / dl;
    double a = dot(up, dh), b = dot(up, perp(dh));
    if (b < 0) {
        d.upsideDown = !d.upsideDown;
        a = -a;
        b = -b;
    }
    if (!(b > 1e-6))
        return false;
    d.position = t.map(d.position, elev);
    d.alignPoint = t.map(d.alignPoint, elev);
    d.rotation = std::atan2(dh.y, dh.x);
    d.widthFactor *= dl / b;
    d.height *= b;
    d.oblique = std::clamp(std::atan(a / b), -85 * kDeg, 85 * kDeg);
    return true;
}

// MTEXT keeps its insertion point and direction in world coordinates: only the height and the
// wrapping width follow the plane. False when the text is seen edge-on.
bool Importer::projectMText(MTextData& d, const Record& r, const Tilt& t) const {
    double dir3[3];
    if (r.has(11)) {
        dir3[0] = r.num(11);
        dir3[1] = r.num(21);
        dir3[2] = r.num(31);
    } else {
        const double c = std::cos(r.num(50) * kDeg), s = std::sin(r.num(50) * kDeg);
        for (int i = 0; i < 3; ++i)
            dir3[i] = t.ax[i] * c + t.ay[i] * s;
    }
    const double l3 = std::sqrt(dir3[0] * dir3[0] + dir3[1] * dir3[1] + dir3[2] * dir3[2]);
    if (!(l3 > 0))
        return false;
    for (double& v : dir3)
        v /= l3;
    const Vec2 dir{dir3[0], dir3[1]};
    const Vec2 up{t.n[1] * dir3[2] - t.n[2] * dir3[1], t.n[2] * dir3[0] - t.n[0] * dir3[2]};
    const double dl = length(dir);
    if (!(dl > 1e-6))
        return false;
    const double b = std::abs(dot(up, perp(dir / dl)));
    if (!(b > 1e-6))
        return false;
    d.rotation = std::atan2(dir.y, dir.x);
    d.height *= b;
    d.width *= dl;
    return true;
}

bool Importer::convTilted(const Record& r, const Tilt& t, std::vector<Entity>& out) {
    const std::string_view type = r.type;
    const std::size_t before = out.size();
    if (type == "CIRCLE" || type == "ARC") {
        const double rad = r.num(40) * k_;
        if (!(rad > 0) || !std::isfinite(rad))
            return false;
        const Vec2 cc = t.map(P(r, 10, 20), r.num(30) * k_);
        const double c[3] = {cc.x, cc.y, 0};
        const double u[3] = {t.ax[0] * rad, t.ax[1] * rad, 0};
        const double v[3] = {t.ay[0] * rad, t.ay[1] * rad, 0};
        double s = 0, e = kTwoPi;
        const bool full = type == "CIRCLE";
        if (!full) {
            s = r.num(50) * kDeg;
            e = r.num(51) * kDeg;
            while (e <= s)
                e += kTwoPi;
        }
        addProjectedEllipse(c, u, v, s, e, full, out);
    } else if (type == "ELLIPSE") {
        // An ellipse is stored in world coordinates; its minor axis is N x major.
        const double ratio = r.num(40, 1);
        const double c[3] = {r.num(10) * k_, r.num(20) * k_, r.num(30) * k_};
        const double u[3] = {r.num(11) * k_, r.num(21) * k_, r.num(31) * k_};
        if (!(ratio > 0) || !std::isfinite(ratio))
            return false;
        const double v[3] = {ratio * (t.n[1] * u[2] - t.n[2] * u[1]),
                             ratio * (t.n[2] * u[0] - t.n[0] * u[2]),
                             ratio * (t.n[0] * u[1] - t.n[1] * u[0])};
        const double s = r.num(41, 0);
        double e = r.num(42, kTwoPi);
        while (e <= s)
            e += kTwoPi;
        addProjectedEllipse(c, u, v, s, e, e - s >= kTwoPi - 1e-9, out);
    } else if (type == "LWPOLYLINE") {
        PolylineData pl;
        for (const Group& g : r.g) {
            if (g.code == 10)
                pl.v.push_back({{g.number() * k_, 0}, 0, 0, 0});
            else if (g.code == 20 && !pl.v.empty())
                pl.v.back().p.y = g.number() * k_;
            else if (g.code == 42 && !pl.v.empty())
                pl.v.back().bulge = g.number();
        }
        if (pl.v.size() < 2)
            return false;
        pl.closed = (r.integer(70) & 1) != 0;
        pl.v = projectPath(pl.v, pl.closed, t, r.num(38) * k_);
        out.push_back(makeEntity(std::move(pl)));
    } else {
        return false;
    }
    if (out.size() == before)
        return false;
    countProjected(r);
    return true;
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

void Importer::add(Entity&& e, BlockId owner, std::string_view type, int line,
                   std::optional<std::uint64_t> handle, const Record* source) {
    EntityCount& cnt = sum_.entities[std::string(type)];
    const EntityCommon common = e.common;
    tbcad::Result<EntityId> res = tx_->add(std::move(e), owner);
    if (res) {
        ++cnt.imported;
        if (handle && *handle != 0)
            srcHandles_.emplace_back(res.value().value, *handle);
        return;
    }
    if (res.error().code == ErrorCode::OutOfRange) {
        ++sum_.beyondHardCap;
        // AutoCAD keeps objects however far away they are. Such an object cannot be drawn here,
        // but its record is kept as it was and written back when the drawing is saved as DXF.
        std::vector<Entity> kept;
        if (source && keepUnshown(*source, kept)) {
            kept[0].common = common;
            if (tx_->add(std::move(kept[0]), owner)) {
                ++cnt.imported;
                report_.warn(code::kBeyondHardCap,
                             std::string(type) +
                                 ": coordinates beyond the hard limit of 1e10 mm; kept for "
                                 "saving, not shown",
                             line, handle);
                return;
            }
        }
        ++cnt.skipped;
        report_.warn(code::kBeyondHardCap,
                     std::string(type) + ": coordinates beyond the hard limit of 1e10 mm; dropped",
                     line, handle);
    } else {
        ++cnt.skipped;
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
    std::optional<std::uint64_t> handle = handleOf(r);  // (only the first entity of a record)
    // A record that made one object can be kept as it was if that object cannot be stored.
    const Record* source = out.size() == 1 ? &r : nullptr;
    for (Entity& e : out) {
        if (!e.common.layer) {
            const bool wipeout = e.common.wipeout;
            e.common = c;
            if (wipeout)
                e.common.wipeout = true;
        }
        add(std::move(e), owner, type, r.line, handle, source);
        handle.reset();
    }
}

bool Importer::convert(Record& r, RecordReader& rr, std::vector<Entity>& out, BlockId owner) {
    std::string_view t = r.type;
    Frame f;
    auto withFrame = [&](auto&& fn) {
        if (frameOf(r, f, t))
            fn();
    };
    if (t == "CIRCLE" || t == "ARC" || t == "ELLIPSE" || t == "LWPOLYLINE") {
        if (const std::optional<Tilt> tilt = tiltOf(r)) {
            if (!convTilted(r, *tilt, out))
                bad(r, t, "degenerate in its tilted plane; skipped");
            return true;
        }
    }
    if (t == "HATCH" || t == "SOLID" || t == "TRACE") {
        if (const std::optional<Tilt> tilt = tiltOf(r)) {
            // Built in object coordinates, then projected.
            const std::size_t before = out.size();
            if (t == "HATCH")
                convHatch(r, f, out);
            else
                convSolid(r, f, out);
            for (std::size_t i = before; i < out.size(); ++i)
                if (auto* h = std::get_if<Indirect<HatchData>>(&out[i].data))
                    projectHatch(**h, *tilt, r.num(30) * k_);
            if (out.size() > before)
                countProjected(r);
            return true;
        }
    }
    if (t == "TEXT" || t == "ATTRIB" || t == "ATTDEF" || t == "MTEXT") {
        if (const std::optional<Tilt> tilt = tiltOf(r)) {
            const std::size_t before = out.size();
            if (t == "MTEXT") {
                convMText(r, f, out);
            } else {
                const TextKind kind = t == "TEXT"     ? TextKind::Text
                                      : t == "ATTRIB" ? TextKind::Attrib
                                                      : TextKind::AttDef;
                convText(r, f, kind, out);
            }
            bool ok = out.size() > before;
            for (std::size_t i = before; i < out.size() && ok; ++i) {
                if (auto* d = std::get_if<Indirect<TextData>>(&out[i].data))
                    ok = projectText(**d, *tilt, r.num(30) * k_);
                else if (auto* m = std::get_if<Indirect<MTextData>>(&out[i].data))
                    ok = projectMText(**m, r, *tilt);
            }
            if (ok) {
                countProjected(r);
            } else if (out.size() > before) {
                out.resize(before);
                bad(r, t, "seen edge-on in its tilted plane; skipped");
            }
            return true;
        }
    }
    if (t == "INSERT") {
        if (const std::optional<Tilt> tilt = tiltOf(r)) {
            // Seen from above the plane is squashed across its horizontal axis: rotation(theta)
            // times scale(1, nz). A block turned by a multiple of 90 degrees in that plane stays a
            // rotated and scaled block; any other turn would need a shear.
            const std::size_t before = out.size();
            convInsert(r, rr, f, out);  // also consumes the ATTRIB records
            const double q = r.num(50) / 90;
            const double k = std::round(q);
            if (std::abs(q - k) > 1e-6) {
                out.resize(before);
                report_.warn(code::kExtrusion3d,
                             "INSERT turned by " + formatDouble(r.num(50)) +
                                 " degrees in a tilted 3D plane cannot be drawn as seen from "
                                 "above; skipped",
                             r.line, handleOf(r));
                return true;
            }
            const double theta = std::atan2(tilt->ax[1], tilt->ax[0]);
            const bool odd = std::fmod(std::abs(k), 2.0) == 1;
            const Vec2 squash = odd ? Vec2{tilt->n[2], 1} : Vec2{1, tilt->n[2]};
            const double elev = r.num(30) * k_;
            for (std::size_t i = before; i < out.size(); ++i) {
                if (auto* d = std::get_if<Indirect<InsertData>>(&out[i].data)) {
                    InsertData& in = **d;
                    in.position = tilt->map(in.position, elev);
                    in.rotation += theta;
                    in.scale = Vec2{in.scale.x * squash.x, in.scale.y * squash.y};
                }
            }
            if (out.size() > before)
                countProjected(r);
            return true;
        }
    }
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
    } else if (t == "PDFUNDERLAY") {
        convPdfUnderlay(r, out);
    } else if (t == "TOLERANCE") {
        convTolerance(r, out);
    } else if (t == "ARC_DIMENSION") {
        convArcDimension(r, out);
    } else if (t == "ACAD_PROXY_ENTITY") {
        convProxy(r, out);
    } else if (t == "ACAD_TABLE") {
        if (!convAcadTable(r, rr, f, out) && !convForeign(r, out))
            return false;
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
    pl.restartLinetype = (r.integer(70) & 128) == 0;  // flag 128: the linetype runs on
    pl.constantWidth = std::max(0.0, r.num(43) * k_);
    if (pl.v.size() < 2) {
        // AutoCAD keeps such a polyline (it draws as nothing, or a dot): so does a save to DXF.
        if (keepUnshown(r, out)) {
            report_.warn(code::kBadEntity,
                         "LWPOLYLINE with fewer than two vertices; kept for saving, not shown",
                         r.line, handleOf(r));
            return true;
        }
        bad(r, "LWPOLYLINE", "fewer than two vertices; skipped");
        return false;
    }
    out.push_back(makeEntity(std::move(pl)));
    return true;
}

// A polygon mesh (M x N grid of vertices) or polyface mesh (vertices plus faces of up to four
// vertex indices, negative for a hidden edge), drawn as its visible edges seen from above.
bool Importer::convMesh(const Record& r, const std::vector<Vec2>& ctrl,
                        const std::vector<Vec2>& fit,
                        const std::vector<std::array<std::int64_t, 4>>& faces,
                        std::vector<Entity>& out) {
    const std::int64_t flags = r.integer(70);
    const std::size_t before = out.size();
    if (flags & 64) {  // polyface
        const std::vector<Vec2>& vs = fit.empty() ? ctrl : fit;
        std::set<std::pair<std::int64_t, std::int64_t>> seen;
        for (const auto& f : faces) {
            int n = 0;
            while (n < 4 && f[n] != 0)
                ++n;
            if (n < 2)
                continue;
            for (int i = 0; i < n; ++i) {
                const std::int64_t a = f[i], b = std::abs(f[(i + 1) % n]);
                if (a < 0 || (n == 2 && i == 1))
                    continue;  // hidden edge (the sign of the edge's first vertex)
                const std::int64_t lo = std::min(a, b), hi = std::max(a, b);
                if (lo < 1 || hi > static_cast<std::int64_t>(vs.size()) || lo == hi ||
                    !seen.insert({lo, hi}).second)
                    continue;
                out.push_back(makeEntity(LineData{vs[static_cast<std::size_t>(a - 1)],
                                                  vs[static_cast<std::size_t>(b - 1)]}));
            }
        }
    } else {  // polygon mesh
        std::int64_t m = r.integer(71), n = r.integer(72);
        const std::int64_t sm = r.integer(73), sn = r.integer(74);
        const std::vector<Vec2>* vs = nullptr;
        bool closedM = (flags & 1) != 0, closedN = (flags & 32) != 0;
        if (!fit.empty() && sm > 1 && sn > 1 && static_cast<std::int64_t>(fit.size()) == sm * sn) {
            vs = &fit;  // a smoothed surface: draw its fitted grid
            m = sm;
            n = sn;
        } else if (m > 0 && n > 0 && static_cast<std::int64_t>(fit.size()) == m * n) {
            vs = &fit;
        } else if (m > 0 && n > 0 && static_cast<std::int64_t>(ctrl.size()) == m * n) {
            vs = &ctrl;
        }
        if (vs && m * n >= 2) {
            auto at = [&](std::int64_t i, std::int64_t j) {
                return (*vs)[static_cast<std::size_t>(i * n + j)];
            };
            auto line = [&](std::vector<PolyVertex> pts, bool closed) {
                if (pts.size() < 2)
                    return;
                PolylineData pl;
                pl.v = std::move(pts);
                pl.closed = closed && pl.v.size() > 2;
                out.push_back(makeEntity(std::move(pl)));
            };
            for (std::int64_t i = 0; i < m; ++i) {
                std::vector<PolyVertex> row;
                for (std::int64_t j = 0; j < n; ++j)
                    row.push_back({at(i, j), 0, 0, 0});
                line(std::move(row), closedN);
            }
            for (std::int64_t j = 0; j < n; ++j) {
                std::vector<PolyVertex> col;
                for (std::int64_t i = 0; i < m; ++i)
                    col.push_back({at(i, j), 0, 0, 0});
                line(std::move(col), closedM);
            }
        }
    }
    if (out.size() == before) {
        bad(r, "POLYLINE", "mesh without usable vertices or faces; skipped");
        return false;
    }
    if (meshes_++ == 0)
        firstMeshLine_ = r.line;
    return true;
}

bool Importer::convPolyline(const Record& r, RecordReader& rr, const Frame& /*unused*/,
                            std::vector<Entity>& out) {
    const std::int64_t flags = r.integer(70);
    const bool mesh = (flags & (16 | 64)) != 0;  // polygon mesh, polyface mesh
    const bool line3d = (flags & 8) != 0 && !mesh;  // 3D polyline: drawn flat, Z dropped
    const double defStart = r.num(40) * k_, defEnd = r.num(41) * k_;

    std::vector<PolyVertex> plain, splineVerts;
    std::vector<Vec2> meshCtrl, meshFit;           // polygon mesh / polyface vertices
    std::vector<std::array<std::int64_t, 4>> faces;  // polyface faces (1-based, < 0 = hidden)
    bool zSeen = false;
    Record v;
    while (rr.next(v)) {
        if (v.type == "VERTEX") {
            std::int64_t vf = v.integer(70);
            if (mesh) {
                if ((vf & 128) && !(vf & 64))
                    faces.push_back({v.integer(71), v.integer(72), v.integer(73), v.integer(74)});
                else
                    ((vf & 16) ? meshCtrl : meshFit).push_back(P(v, 10, 20));
                continue;
            }
            if (vf & 16)
                continue;  // spline frame control point
            if (line3d && !zSeen && std::abs(v.num(30)) > 1e-9) {
                zSeen = true;
                if (flattened_++ == 0)
                    firstFlattenedLine_ = v.line;
            }
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
    if (mesh)
        return convMesh(r, meshCtrl, meshFit, faces, out);
    Frame f;  // a 3D polyline is in world coordinates; its vertices have no extrusion
    const std::optional<Tilt> tilt = line3d ? std::nullopt : tiltOf(r);
    if (!line3d && !tilt && !frameOf(r, f, "POLYLINE"))
        return false;
    PolylineData pl;
    pl.v = ((flags & (2 | 4)) && !splineVerts.empty()) ? std::move(splineVerts) : std::move(plain);
    for (PolyVertex& pv : pl.v) {
        pv.p = f.p(pv.p);
        pv.bulge = f.bulge(pv.bulge);
    }
    pl.closed = (flags & 1) != 0;
    pl.restartLinetype = (flags & 128) == 0;
    if (pl.v.size() < 2) {
        bad(r, "POLYLINE", "fewer than two vertices; skipped");
        return false;
    }
    if (tilt) {
        pl.v = projectPath(pl.v, pl.closed, *tilt, r.num(30) * k_);
        countProjected(r);
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
    {
        // Group 71 is the object's effective flags; ours are relative to the style's.
        const TextStyle* ts = doc_->textStyle(d.style);
        d.backward = ((r.integer(71) & 2) != 0) != (ts && ts->backward);
        d.upsideDown = ((r.integer(71) & 4) != 0) != (ts && ts->upsideDown);
    }
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
    if (r.has(44) && r.num(44) >= 0.25 && r.num(44) <= 4.0)
        d.lineSpacing = r.num(44);
    if (const std::int64_t fill = r.integer(90, 0); fill >= 1 && fill <= 3) {
        // 1 = fill colour (63), 2 / 3 = the drawing / window background.
        const std::int64_t aci = r.integer(63, 0);
        d.bgMode = fill == 1 && aci >= 1 && aci <= 255 ? 2 : 1;
        if (d.bgMode == 2)
            d.bgColor = static_cast<int>(aci);
        if (r.has(45) && r.num(45) >= 1.0 && r.num(45) <= 5.0)
            d.bgScale = r.num(45);
    }
    if (r.integer(75, 0) > 0 && r.integer(76, 1) > 1) {
        d.columns = static_cast<std::uint8_t>(std::clamp<std::int64_t>(r.integer(76, 1), 1, 16));
        if (r.has(48) && r.num(48) > 0)
            d.width = r.num(48) * k_;
        d.columnGap = std::max(0.0, r.num(49) * k_);
    }
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

// An ATTRIB that sits away from its definition's place was moved by hand: keep that as the
// attribute's offset (block coordinates).
void Importer::attribOffsets(InsertData& d, const std::vector<Vec2>& anchors) const {
    const BlockDef* def = doc_->block(d.block);
    if (!def)
        return;
    const double c = std::cos(d.rotation), s = std::sin(d.rotation);
    for (std::size_t i = 0; i < d.attribs.size() && i < anchors.size(); ++i) {
        AttribValue& v = d.attribs[i];
        for (EntityId id : def->entities) {
            const Entity* e = doc_->find(id);
            const auto* t = e ? entityAs<TextData>(*e) : nullptr;
            if (!t || !t->attdef || t->attdef->tag != v.tag)
                continue;
            const Vec2 rel = anchors[i] - d.position;  // world -> block, undoing the placement
            const Vec2 local = def->base + Vec2{(rel.x * c + rel.y * s) / d.scale.x,
                                                (-rel.x * s + rel.y * c) / d.scale.y};
            const Vec2 home =
                (t->h == HAlign::Left && t->v == VAlign::Baseline) ? t->position : t->alignPoint;
            const Vec2 off = local - home;
            if (std::isfinite(off.x) && std::isfinite(off.y) &&
                (std::abs(off.x) > 1e-6 || std::abs(off.y) > 1e-6))
                v.offset = off;
            break;
        }
    }
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
    std::vector<Vec2> valueAnchors;  // where each ATTRIB sits (drawing coordinates)
    std::vector<Entity> loose;
    for (const Record& a : attribs) {
        std::string tag = upperAscii(dec(a.raw(2)));
        std::string value = replacePercentCodes(dec(a.raw(1)), true);
        if (blockHasAttDef(block, tag)) {
            bool seen = false;
            for (const AttribValue& v : values)
                seen = seen || v.tag == tag;
            if (!seen) {
                const bool anchored = a.integer(72) != 0 || a.integer(74) != 0;
                values.push_back({std::move(tag), std::move(value), {}});
                valueAnchors.push_back(f.p(anchored ? P(a, 11, 21) : P(a, 10, 20)));
            }
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
    bool clipInvert = false;
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
                else if (g.str() == "clipinvert=1")
                    clipInvert = true;
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
            if (rows * cols == 1)
                attribOffsets(d, valueAnchors);
            d.clip = clip;
            d.clipOn = clipOn;
            d.clipInvert = clipInvert;
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
    bool gaps = false;
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
            if (g.integer() > 2)
                gaps = true;  // an element with gaps (MLEDIT cuts)
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

    // Keep it a multiline when its style is in the file and the style reproduces the lines.
    if (!gaps && (r.integer(71) & 12) == 0) {
        loadMlineStyles();
        const std::string name = dec(r.raw(2));
        std::string key = name;
        for (char& c : key)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        auto sit = mlStyles_.find(key);
        if (sit != mlStyles_.end() && sit->second.offsets.size() == elements) {
            MlineData md;
            md.style = sit->second.name;
            md.scale = r.num(40, 1);
            md.justification = static_cast<int>(r.integer(70, 1));
            md.closed = closed && vs.size() > 2;
            for (const Vertex& v : vs)
                md.v.push_back(v.pos);
            if (md.scale > 0 && md.justification >= 0 && md.justification <= 2) {
                const mline::Strokes st = mline::strokes(md, sit->second);
                bool same = st.lines.size() >= elements;
                for (std::size_t e = 0; same && e < elements; ++e) {
                    const auto& line = st.lines[e];
                    same =
                        line.size() == vs.size() + (md.closed ? 1 : 0) || line.size() == vs.size();
                    for (std::size_t i = 0; same && i < vs.size() && i < line.size(); ++i) {
                        const Vec2 want = vs[i].pos + vs[i].miter * vs[i].offset[e];
                        same = distance(want, line[i]) <= 1e-4 * std::max(1.0, md.scale);
                    }
                }
                if (same) {
                    (void)tx_->modifyHeader([&](Header& h) { mline::storeStyle(h, sit->second); });
                    out.push_back(makeEntity(Indirect<MlineData>(std::move(md))));
                    return true;
                }
            }
        }
    }
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
// The clip (rectangle or polygon), fade, brightness and contrast are kept.
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
    img.transparent = (r.integer(70, 0) & 8) != 0;
    img.brightness = static_cast<int>(std::clamp(r.num(281, 50), 0.0, 100.0));
    img.contrast = static_cast<int>(std::clamp(r.num(282, 50), 0.0, 100.0));
    if (r.integer(71) == 2 && clip.size() >= 3) {
        // A polygon (AutoCAD closes it by repeating the first point).
        if (clip.size() > 3 && tol::nearlyEqual(clip.front(), clip.back()))
            clip.pop_back();
        const bool box = clip.size() == 4 && ((clip[0].x == clip[1].x) != (clip[0].y == clip[1].y));
        if (!box)
            for (const Vec2& c : clip)
                img.clip.push_back({std::clamp((c.x + 0.5) / w, 0.0, 1.0),
                                    std::clamp(1.0 - (c.y + 0.5) / h, 0.0, 1.0)});
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
            img.clip.clear();
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

// PDFUNDERLAY: a PDF page placed in the drawing. It becomes the same thing PDFATTACH makes: a block
// that is an external reference to "file.pdf#page=N,fade=F", inserted with the underlay's scale.
// The file name and page are in the PDFDEFINITION object, read later (resolveImages); until the
// host loads the page, a unit square stands in for it.
bool Importer::convPdfUnderlay(const Record& r, std::vector<Entity>& out) {
    const double sx = r.num(41, 1), sy = r.num(42, 1);
    if (!(sx > 0) || !(sy > 0) || !std::isfinite(sx) || !std::isfinite(sy))
        return convForeign(r, out);
    std::string handle(r.raw(340));
    std::string base = "PDFUNDERLAY_" + (handle.empty() ? std::string("0") : handle);
    const int n = imageNames_[base]++;
    const std::string name = n == 0 ? base : base + "_" + std::to_string(n + 1);
    const BlockId block = blockFor(name);
    if (!block)
        return false;
    blockDefined_.insert(block);
    ++sum_.blocks;

    PolylineData frame;
    frame.closed = true;
    for (const Vec2 p : {Vec2{0, 0}, Vec2{1, 0}, Vec2{1, 1}, Vec2{0, 1}})
        frame.v.push_back({p, 0, 0, 0});
    Entity fe = makeEntity(std::move(frame));
    fe.common = commonOf(r);
    (void)tx_->add(std::move(fe), block);

    PendingPdf pdf;
    pdf.block = block;
    pdf.name = name;
    pdf.defHandle = handle;
    pdf.line = r.line;
    pdf.fade = std::clamp(r.num(282, 25), 0.0, 90.0);
    pdf.contrast = static_cast<int>(std::clamp(r.num(281, 50), 0.0, 100.0));
    pdf.monochrome = (r.integer(280, 0) & 4) != 0;
    pdfs_.push_back(std::move(pdf));

    InsertData d;
    d.block = block;
    d.position = P(r, 10, 20);
    d.scale = {sx, sy};
    d.rotation = r.num(50) * kDeg;
    out.push_back(makeEntity(std::move(d)));
    return true;
}

// Gives the picture blocks their file (IMAGEDEF) and display settings (fade, clip).
void Importer::resolveImages() {
    for (const PendingPdf& pdf : pdfs_) {
        auto it = pdfDefs_.find(pdf.defHandle);
        if (it == pdfDefs_.end() || it->second.file.empty()) {
            report_.warn(code::kXref,
                         "PDFUNDERLAY \"" + pdf.name + "\" has no PDF file (PDFDEFINITION)",
                         pdf.line);
            continue;
        }
        std::string path = it->second.file;
        std::replace(path.begin(), path.end(), '\\', '/');
        underlay::PdfOptions o;
        o.page = it->second.page;
        o.fadePercent = pdf.fade;
        o.contrast = pdf.contrast;
        o.monochrome = pdf.monochrome;
        const std::string stored = underlay::makeSpec(path, o);
        (void)tx_->modifyBlock(pdf.block, [&](BlockDef& b) { b.xrefPath = stored; });
    }
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
        p.transparent = img.transparent;
        p.brightness = img.brightness;
        p.contrast = img.contrast;
        p.clip = img.clip;
        if (imageFrame_ >= 0)
            p.frame = imageFrame_;
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
// AutoCAD keeps the lines and text of a TABLE in the anonymous block named by group 2 (*T1, ...),
// in world coordinates, so an INSERT of that block at the origin draws the table as it was. The
// cell data (groups 91, 92, 301...) is not read: the table is no longer editable as a table.
// The cells of an AutoCAD table, read into an editable TBCAD table: row heights (141), column
// widths (142), then one run of groups per cell, row by row, each starting with 171 (cell type).
// A cell has its text in 1 (after 2/3 pieces) or, in newer files, in the 302 of its CELL_VALUE;
// 175/176 are the columns and rows a merged cell spans; 170 is its alignment (1..9, TL..BR) and
// 140 its text height. Anything that does not add up returns nothing, and the table is shown
// from its block instead.
std::optional<TableData> Importer::acadTableData(const Record& r, const Frame& f) {
    if (f.mirror)
        return std::nullopt;
    std::size_t at = 0;
    while (at < r.g.size() && !(r.g[at].code == 100 && r.g[at].str() == "AcDbTable"))
        ++at;
    if (at == r.g.size())
        return std::nullopt;
    TableData t;
    std::int64_t rows = -1, cols = -1;
    Vec2 dir{1, 0};
    std::size_t i = at + 1;
    for (; i < r.g.size() && r.g[i].code != 171; ++i) {
        const Group& g = r.g[i];
        switch (g.code) {
            case 91:
                if (rows < 0)
                    rows = g.integer();
                break;
            case 92:
                if (cols < 0)
                    cols = g.integer();
                break;
            case 11:
                dir.x = g.number();
                break;
            case 21:
                dir.y = g.number();
                break;
            case 141:
                t.rowHeights.push_back(g.number() * k_);
                break;
            case 142:
                t.colWidths.push_back(g.number() * k_);
                break;
            default:
                break;
        }
    }
    if (rows < 1 || cols < 1 || rows * cols > 100000 ||
        t.rowHeights.size() != static_cast<std::size_t>(rows) ||
        t.colWidths.size() != static_cast<std::size_t>(cols))
        return std::nullopt;
    for (double v : t.rowHeights)
        if (!(v > 0) || !std::isfinite(v))
            return std::nullopt;
    for (double v : t.colWidths)
        if (!(v > 0) || !std::isfinite(v))
            return std::nullopt;

    struct Cell {
        std::string text{};
        std::string value{};  // the first 302 of a CELL_VALUE
        int colSpan = 1, rowSpan = 1;
        int align = 0;
        double height = 0;
        std::string style{};  // group 7
    };
    std::vector<Cell> cells;
    for (; i < r.g.size(); ++i) {
        const Group& g = r.g[i];
        if (g.code == 171) {
            cells.emplace_back();
            continue;
        }
        if (cells.empty())
            continue;
        Cell& c = cells.back();
        switch (g.code) {
            case 1:
            case 2:
            case 3:
                c.text += dec(g.str());
                break;
            case 302:
                if (c.value.empty())
                    c.value = dec(g.str());
                break;
            case 175:
                c.colSpan = static_cast<int>(std::clamp<std::int64_t>(g.integer(), 1, cols));
                break;
            case 176:
                c.rowSpan = static_cast<int>(std::clamp<std::int64_t>(g.integer(), 1, rows));
                break;
            case 170:
                if (c.align == 0)
                    c.align = static_cast<int>(g.integer());
                break;
            case 140:
                if (c.height == 0 && std::isfinite(g.number()) && g.number() > 0)
                    c.height = g.number() * k_;
                break;
            case 7:
                if (c.style.empty())
                    c.style = std::string(g.str());
                break;
            default:
                break;
        }
    }
    if (cells.size() != static_cast<std::size_t>(rows * cols))
        return std::nullopt;

    t.cells.resize(cells.size());
    std::vector<double> heights;
    std::map<int, int> dataAlign;
    for (std::int64_t row = 0; row < rows; ++row) {
        for (std::int64_t col = 0; col < cols; ++col) {
            const std::size_t k = static_cast<std::size_t>(row * cols + col);
            const Cell& c = cells[k];
            TableCell& tc = t.cells[k];
            if (tc.covered)
                continue;
            tc.utf8 = c.text.empty() ? c.value : c.text;
            const int cs = static_cast<int>(std::min<std::int64_t>(c.colSpan, cols - col));
            const int rs = static_cast<int>(std::min<std::int64_t>(c.rowSpan, rows - row));
            tc.colSpan = static_cast<std::uint16_t>(cs);
            tc.rowSpan = static_cast<std::uint16_t>(rs);
            for (int dr = 0; dr < rs; ++dr)
                for (int dc = 0; dc < cs; ++dc)
                    if (dr || dc) {
                        TableCell& cov = t.cells[k + dr * cols + dc];
                        cov.covered = true;
                        cov.utf8.clear();
                    }
            if (c.height > 0)
                heights.push_back(c.height);
            if (row > 0 && c.align >= 1 && c.align <= 9)
                ++dataAlign[(c.align - 1) % 3];
        }
    }
    if (!heights.empty()) {
        std::nth_element(heights.begin(), heights.begin() + heights.size() / 2, heights.end());
        t.textHeight = heights[heights.size() / 2];
    }
    // A first row that is one cell across the whole table is a title.
    t.titleRow = rows > 1 && cols > 1 && t.cells[0].colSpan == cols;
    if (t.titleRow && cells[0].height > 0 && std::abs(cells[0].height - t.textHeight) > 1e-9)
        t.titleStyle.textHeight = cells[0].height;
    if (!dataAlign.empty()) {
        auto best = std::max_element(dataAlign.begin(), dataAlign.end(),
                                     [](auto& a, auto& b) { return a.second < b.second; });
        t.dataStyle.align = best->first;
    }
    t.margin = std::clamp(t.textHeight * 0.4, 0.0, 10.0);
    for (const Cell& c : cells)
        if (!c.style.empty()) {
            t.style = textStyleFor(c.style);
            break;
        }
    t.origin = f.p(P(r, 10, 20));
    t.rotation = f.angle(std::atan2(dir.y, dir.x));
    return t;
}

bool Importer::convAcadTable(const Record& r, RecordReader& rr, const Frame& f,
                             std::vector<Entity>& out) {
    if (auto t = acadTableData(r, f)) {
        out.push_back(makeEntity(std::move(*t)));
        ++acadTables_;
        return true;
    }
    if (r.raw(2).empty())
        return false;
    Record ins;
    ins.type = "INSERT";
    ins.line = r.line;
    auto number = [&](int code, double v) {
        Group g;
        g.code = code;
        g.value = v;
        g.line = r.line;
        ins.g.push_back(std::move(g));
    };
    for (const Group& g : r.g) {
        switch (g.code) {
            case 2:    // the block
            case 5:    // handle
            case 6:    // linetype
            case 8:    // layer
            case 48:   // linetype scale
            case 60:   // visibility
            case 62:   // colour
            case 67:   // space
            case 370:  // lineweight
            case 420:  // true colour
            case 440:  // transparency
                ins.g.push_back(g);
                break;
            default:
                break;
        }
    }
    number(10, 0.0);
    number(20, 0.0);
    number(30, 0.0);
    number(41, 1.0);
    number(42, 1.0);
    number(43, 1.0);
    number(50, 0.0);
    if (!convInsert(ins, rr, f, out) || out.empty())
        return false;
    return true;
}

std::string Importer::keptRecord(const Record& r) {
    // Valid while the units were not scaled.
    if (k_ != 1)
        return {};
    std::string body;
    for (const Group& g : r.g) {
        const int c = g.code;
        if (c == 5 || c == 6 || c == 8 || c == 48 || c == 60 || c == 62 || c == 67 || c == 102 ||
            c == 284 || c == 370 || c == 410 || c == 420 || c == 430 || c == 440 || c >= 1000 ||
            handleCode(c) || (c >= 480 && c <= 481) || (c == 100 && g.str() == "AcDbEntity"))
            continue;
        // The ATTRIB records that followed an INSERT are not kept with it, so it no longer says
        // they follow.
        if (c == 66 && r.type == "INSERT")
            continue;
        std::string text;
        if (!keptValue(g, [&](std::string_view v) { return dec(v); }, text))
            return {};
        packGroup(body, c, text);
    }
    return body.empty() ? std::string{} : std::string(kProxyDxfFresh) + body;
}

bool Importer::keepUnshown(const Record& r, std::vector<Entity>& out) {
    const std::string kept = keptRecord(r);
    if (kept.empty())
        return false;
    ProxyData d;
    d.originalType = std::string(r.type);
    d.raw.assign(kept.begin(), kept.end());
    out.push_back(makeEntity(std::move(d)));
    return true;
}

bool Importer::convForeign(const Record& r, std::vector<Entity>& out) {
    ProxyData d;
    d.originalType = std::string(r.type);
    std::vector<Entity> texts;
    std::size_t unknown = 0;
    std::string problem;
    std::vector<std::uint8_t> gfx = proxyGraphicsOf(r);
    if (gfx.size() >= 16)
        unknown = decodeProxyGraphics(gfx, commonOf(r), d, texts, problem);
    if (d.graphics.items.empty() && r.type == "MESH")
        meshEdges(r, commonOf(r), d);

    // The record itself, to be written back unchanged.
    const std::string kept = keptRecord(r);
    if (!kept.empty())
        d.raw.assign(kept.begin(), kept.end());
    if (d.graphics.items.empty() && texts.empty() && kept.empty())
        return false;
    std::string type(r.type);
    if (foreignShown_.insert(type).second) {
        if (!d.graphics.items.empty() || !texts.empty()) {
            report_.info(
                code::kProxyGraphics,
                type + ": no reader for this object; shown from its proxy graphics (read-only)",
                r.line);
        } else {
            report_.info(code::kProxyGraphics,
                         type +
                             ": no reader for this object; kept as it is and written back to "
                             "DXF (not shown)",
                         r.line);
        }
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
