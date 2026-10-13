// DXF import: one record -> TBCAD entities (everything except HATCH and DIMENSION).
#include <algorithm>
#include <cmath>

#include "importer.h"
#include "tbcad/geom/spline.h"
#include "tbcad/table/table.h"

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
    if (const Group* g = r.find(48)) {
        double s = g->number();
        if (std::isfinite(s) && s > 0)
            c.linetypeScale = s;
    }
    c.visible = r.integer(60, 0) == 0;
    bool ours = false;
    for (const Group& g : r.g) {
        if (g.code == 1001)
            ours = g.str() == "TBCAD";
        else if (ours && g.code == 1000 && g.str() == "annotative=1")
            c.annotative = true;
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
    if (std::abs(nx) < 1e-6 && std::abs(ny) < 1e-6) {
        f.mirror = nz < 0;
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
    if (r.integer(67) == 1) {
        ++paperSpace_;
        return;
    }
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
    EntityCommon c = commonOf(r);
    for (Entity& e : out) {
        if (!e.common.layer)
            e.common = c;
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
    } else if (t == "ACAD_PROXY_ENTITY") {
        convProxy(r, out);
    } else {
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
    std::string text = replacePercentCodes(dec(r.raw(1)));
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
        std::string value = replacePercentCodes(dec(a.raw(1)));
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
    double cr = std::cos(rot), sr = std::sin(rot);
    for (std::int64_t row = 0; row < rows; ++row) {
        for (std::int64_t col = 0; col < cols; ++col) {
            double ox = static_cast<double>(col) * cs, oy = static_cast<double>(row) * rs;
            Vec2 p = pos + Vec2{ox * cr - oy * sr, ox * sr + oy * cr};
            InsertData d;
            d.block = block;
            d.position = f.p(p);
            d.scale = f.mirror ? Vec2{-sx, sy} : Vec2{sx, sy};
            d.rotation = f.mirror ? -rot : rot;
            d.attribs = values;
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
    report_.warn(code::kProxy,
                 "ACAD_PROXY_ENTITY kept as a read-only placeholder (its graphics are not decoded)",
                 r.line, handleOf(r));
    out.push_back(makeEntity(std::move(d)));
    return true;
}

}  // namespace tbcad::dxf::detail
