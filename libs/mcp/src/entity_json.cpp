#include "tbcad/mcp/entity_json.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <stdexcept>

#include "tbcad/core/units.h"

namespace tbcad::mcp {

namespace {

struct ParseError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

[[noreturn]] void fail(const std::string& message) {
    throw ParseError(message);
}

std::string lower(std::string s) {
    for (char& c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

template <class T>
T orFail(Result<T> r) {
    if (!r)
        fail(r.error().message);
    return std::move(r.value());
}

double num(const Json& j, const char* what) {
    if (j.isNumber() || j.isObject())
        return orFail(lengthFromJson(j));
    fail(std::string(what) + " must be a number");
}

double angle(const Json& j, const char* what) {
    if (j.isNumber() || j.isObject())
        return orFail(angleFromJson(j));
    fail(std::string(what) + " must be an angle (radians or {\"deg\": n})");
}

const Json& req(const Json& o, const char* key) {
    const Json* v = o.find(key);
    if (!v)
        fail(std::string("missing \"") + key + "\"");
    return *v;
}

Vec2 pt(const Json& o, const char* key) {
    return orFail(pointFromJson(req(o, key)));
}

double lenField(const Json& o, const char* key) {
    return num(req(o, key), key);
}

double lenOr(const Json& o, const char* key, double fallback) {
    const Json* v = o.find(key);
    return v && !v->isNull() ? num(*v, key) : fallback;
}

double angOr(const Json& o, const char* key, double fallback) {
    const Json* v = o.find(key);
    return v && !v->isNull() ? angle(*v, key) : fallback;
}

bool boolOr(const Json& o, const char* key, bool fallback) {
    const Json* v = o.find(key);
    return v && v->isBool() ? v->asBool() : fallback;
}

std::string strOr(const Json& o, const char* key, std::string fallback = {}) {
    const Json* v = o.find(key);
    return v && v->isString() ? v->asString() : fallback;
}

Json pointsToJson(const std::vector<Vec2>& pts) {
    Json::Array a;
    for (const auto& p : pts)
        a.push_back(pointToJson(p));
    return Json(std::move(a));
}

std::vector<Vec2> pointsFrom(const Json& j, const char* what) {
    if (!j.isArray())
        fail(std::string(what) + " must be an array of points");
    std::vector<Vec2> out;
    for (const auto& e : j.asArray())
        out.push_back(orFail(pointFromJson(e)));
    return out;
}

Json verticesToJson(const std::vector<PolyVertex>& v) {
    bool plain = std::all_of(v.begin(), v.end(), [](const PolyVertex& p) {
        return p.bulge == 0 && p.startWidth == 0 && p.endWidth == 0;
    });
    Json::Array a;
    for (const auto& p : v) {
        if (plain) {
            a.push_back(pointToJson(p.p));
            continue;
        }
        Json::Object o;
        o["x"] = p.p.x;
        o["y"] = p.p.y;
        if (p.bulge != 0)
            o["bulge"] = p.bulge;
        if (p.startWidth != 0)
            o["startWidth"] = p.startWidth;
        if (p.endWidth != 0)
            o["endWidth"] = p.endWidth;
        a.push_back(Json(std::move(o)));
    }
    return Json(std::move(a));
}

std::vector<PolyVertex> verticesFrom(const Json& j) {
    if (!j.isArray())
        fail("vertices must be an array");
    std::vector<PolyVertex> out;
    for (const auto& e : j.asArray()) {
        PolyVertex v;
        if (e.isObject() && e.find("x")) {
            v.p = orFail(pointFromJson(e));
            v.bulge = e.find("bulge") ? e.find("bulge")->asNumber() : 0;
            v.startWidth = lenOr(e, "startWidth", 0);
            v.endWidth = lenOr(e, "endWidth", 0);
        } else {
            v.p = orFail(pointFromJson(e));
        }
        out.push_back(v);
    }
    return out;
}

const char* hAlignName(HAlign h) {
    switch (h) {
        case HAlign::Left:
            return "left";
        case HAlign::Center:
            return "center";
        case HAlign::Right:
            return "right";
        case HAlign::Aligned:
            return "aligned";
        case HAlign::Middle:
            return "middle";
        case HAlign::Fit:
            return "fit";
    }
    return "left";
}
const char* vAlignName(VAlign v) {
    switch (v) {
        case VAlign::Baseline:
            return "baseline";
        case VAlign::Bottom:
            return "bottom";
        case VAlign::Middle:
            return "middle";
        case VAlign::Top:
            return "top";
    }
    return "baseline";
}
const char* const kAttachNames[] = {"topLeft",    "topCenter",    "topRight",
                                    "middleLeft", "middleCenter", "middleRight",
                                    "bottomLeft", "bottomCenter", "bottomRight"};

const char* dimKindName(DimKind k) {
    switch (k) {
        case DimKind::Linear:
            return "linear";
        case DimKind::Aligned:
            return "aligned";
        case DimKind::Radius:
            return "radius";
        case DimKind::Diameter:
            return "diameter";
        case DimKind::Angular2Line:
            return "angular2line";
        case DimKind::Angular3Point:
            return "angular3point";
        case DimKind::ArcLength:
            return "arclength";
    }
    return "linear";
}

Json splineToJson(const SplineData& s) {
    Json::Object o;
    o["degree"] = s.degree;
    o["controlPoints"] = pointsToJson(s.control);
    if (!s.fit.empty())
        o["fitPoints"] = pointsToJson(s.fit);
    if (!s.weights.empty()) {
        Json::Array w;
        for (double d : s.weights)
            w.push_back(d);
        o["weights"] = Json(std::move(w));
    }
    o["closed"] = s.closed;
    return Json(std::move(o));
}

Json edgeToJson(const HatchEdge& e) {
    Json::Object o;
    if (const auto* l = std::get_if<HatchLineEdge>(&e)) {
        o["type"] = "line";
        o["start"] = pointToJson(l->a);
        o["end"] = pointToJson(l->b);
    } else if (const auto* a = std::get_if<HatchArcEdge>(&e)) {
        o["type"] = "arc";
        o["center"] = pointToJson(a->center);
        o["radius"] = a->radius;
        o["startAngle"] = a->startAngle;
        o["endAngle"] = a->endAngle;
        o["ccw"] = a->ccw;
    } else if (const auto* el = std::get_if<HatchEllipseEdge>(&e)) {
        o["type"] = "ellipse";
        o["center"] = pointToJson(el->center);
        o["majorAxis"] = pointToJson(el->majorAxis);
        o["ratio"] = el->ratio;
        o["startParam"] = el->startParam;
        o["endParam"] = el->endParam;
        o["ccw"] = el->ccw;
    } else if (const auto* sp = std::get_if<HatchSplineEdge>(&e)) {
        o["type"] = "spline";
        Json s = splineToJson(sp->spline);
        for (const auto& [k, v] : s.asObject())
            o[k] = v;
    } else if (const auto* pl = std::get_if<HatchPolylineEdge>(&e)) {
        o["type"] = "polyline";
        o["vertices"] = verticesToJson(pl->v);
        o["closed"] = pl->closed;
    }
    return Json(std::move(o));
}

HatchEdge edgeFromJson(const Json& j) {
    std::string type = lower(strOr(j, "type", "polyline"));
    if (type == "line")
        return HatchLineEdge{pt(j, "start"), pt(j, "end")};
    if (type == "arc") {
        HatchArcEdge a;
        a.center = pt(j, "center");
        a.radius = lenField(j, "radius");
        a.startAngle = angle(req(j, "startAngle"), "startAngle");
        a.endAngle = angle(req(j, "endAngle"), "endAngle");
        a.ccw = boolOr(j, "ccw", true);
        return a;
    }
    if (type == "polyline") {
        HatchPolylineEdge p;
        p.v = verticesFrom(req(j, "vertices"));
        p.closed = boolOr(j, "closed", true);
        return p;
    }
    fail("hatch edge type \"" + type + "\" cannot be written; use line, arc or polyline");
}

HatchLoop loopFromJson(const Json& j) {
    HatchLoop loop;
    if (j.isArray()) {  // a plain polygon: [[x,y], ...]
        HatchPolylineEdge p;
        p.v = verticesFrom(j);
        loop.edges.emplace_back(std::move(p));
        return loop;
    }
    if (!j.isObject())
        fail("a hatch loop is an array of points or an object");
    loop.outer = boolOr(j, "outer", true);
    if (const Json* edges = j.find("edges")) {
        for (const auto& e : edges->asArray())
            loop.edges.push_back(edgeFromJson(e));
    } else {
        HatchPolylineEdge p;
        p.v = verticesFrom(req(j, "vertices"));
        p.closed = true;
        loop.edges.emplace_back(std::move(p));
    }
    if (loop.edges.empty())
        fail("a hatch loop needs edges");
    return loop;
}

SplineData splineFromJson(const Json& j) {
    SplineData s;
    s.degree = static_cast<int>(j.find("degree") ? j.find("degree")->asNumber(3) : 3);
    s.closed = boolOr(j, "closed", false);
    if (s.degree < 1 || s.degree > 9)
        fail("spline degree must be 1..9");
    if (j.find("controlPoints")) {
        s.control = pointsFrom(*j.find("controlPoints"), "controlPoints");
        if (const Json* w = j.find("weights")) {
            for (const auto& d : w->asArray())
                s.weights.push_back(d.asNumber(1));
            if (s.weights.size() != s.control.size())
                fail("weights must match controlPoints");
        }
        if (const Json* f = j.find("fitPoints"))
            s.fit = pointsFrom(*f, "fitPoints");
        std::size_t n = s.control.size();
        auto deg = static_cast<std::size_t>(s.degree);
        if (n < deg + 1)
            fail("a degree " + std::to_string(s.degree) + " spline needs at least " +
                 std::to_string(deg + 1) + " control points");
        // Clamped uniform knot vector.
        std::size_t m = n + deg + 1;
        s.knots.assign(m, 0);
        std::size_t inner = n - deg;  // number of spans
        for (std::size_t i = 0; i < m; ++i) {
            if (i <= deg)
                s.knots[i] = 0;
            else if (i >= n)
                s.knots[i] = static_cast<double>(inner);
            else
                s.knots[i] = static_cast<double>(i - deg);
        }
        return s;
    }
    fail(
        "a spline needs \"controlPoints\" (fit-point-only splines are drawn with run_command "
        "SPLINE)");
}

}  // namespace

// ---- Units ------------------------------------------------------------------------------------

Result<double> lengthFromJson(const Json& j) {
    if (j.isNumber()) {
        if (!std::isfinite(j.asNumber()))
            return makeError(ErrorCode::InvalidArgument, "number is not finite");
        return j.asNumber();
    }
    if (j.isObject()) {
        const Json* v = j.find("value");
        if (!v || !v->isNumber())
            return makeError(ErrorCode::InvalidArgument,
                             "a length object needs a numeric \"value\"");
        std::string unit = lower(j.find("unit") ? j.find("unit")->asString() : "mm");
        double f = 1;
        if (unit == "mm" || unit.empty())
            f = 1;
        else if (unit == "cm")
            f = 10;
        else if (unit == "m")
            f = 1000;
        else if (unit == "in" || unit == "inch" || unit == "\"")
            f = 25.4;
        else if (unit == "ft" || unit == "foot" || unit == "'")
            f = 304.8;
        else
            return makeError(ErrorCode::InvalidArgument,
                             "unknown unit \"" + unit + "\" (use mm, cm, m, in or ft)");
        return v->asNumber() * f;
    }
    return makeError(ErrorCode::InvalidArgument, "expected a number or {\"value\", \"unit\"}");
}

Result<double> angleFromJson(const Json& j) {
    if (j.isNumber())
        return j.asNumber();
    if (j.isObject()) {
        if (const Json* d = j.find("deg"); d && d->isNumber())
            return d->asNumber() * kPi / 180.0;
        if (const Json* r = j.find("rad"); r && r->isNumber())
            return r->asNumber();
    }
    return makeError(ErrorCode::InvalidArgument, "expected an angle: radians or {\"deg\": n}");
}

Result<Vec2> pointFromJson(const Json& j) {
    const Json *x = nullptr, *y = nullptr;
    if (j.isArray() && j.asArray().size() == 2) {
        x = &j.asArray()[0];
        y = &j.asArray()[1];
    } else if (j.isObject() && j.find("x") && j.find("y")) {
        x = j.find("x");
        y = j.find("y");
    } else {
        return makeError(ErrorCode::InvalidArgument, "expected a point [x, y]");
    }
    auto px = lengthFromJson(*x);
    if (!px)
        return px.error();
    auto py = lengthFromJson(*y);
    if (!py)
        return py.error();
    return Vec2{px.value(), py.value()};
}

Json pointToJson(Vec2 p) {
    return Json(Json::Array{Json(p.x), Json(p.y)});
}

// ---- Layers -----------------------------------------------------------------------------------

std::string layerPath(const Document& doc, LayerId id) {
    std::vector<std::string> names;
    for (const Layer* l = doc.layer(id); l; l = l->parent ? doc.layer(l->parent) : nullptr)
        names.push_back(l->name);
    std::string out;
    for (auto it = names.rbegin(); it != names.rend(); ++it) {
        if (!out.empty())
            out += '~';
        out += *it;
    }
    return out;
}

namespace {
std::vector<std::string> splitPath(std::string_view path) {
    std::vector<std::string> parts;
    std::string cur;
    for (char c : path) {
        if (c == '~') {
            parts.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    parts.push_back(cur);
    return parts;
}
}  // namespace

const Layer* findLayerByPath(const Document& doc, std::string_view path) {
    const Layer* cur = nullptr;
    for (const auto& name : splitPath(path)) {
        if (name.empty())
            return nullptr;
        cur = doc.layerByName(cur ? cur->id : LayerId{}, name);
        if (!cur)
            return nullptr;
    }
    return cur;
}

Result<LayerId> ensureLayerPath(const Document& doc, Transaction& tx, std::string_view path,
                                std::vector<std::string>* created) {
    LayerId parent{};
    std::string soFar;
    for (const auto& name : splitPath(path)) {
        if (name.empty())
            return makeError(ErrorCode::InvalidArgument,
                             "empty layer name in \"" + std::string(path) + "\"");
        if (!soFar.empty())
            soFar += '~';
        soFar += name;
        if (const Layer* l = doc.layerByName(parent, name)) {
            parent = l->id;
            continue;
        }
        Layer row;
        row.name = name;
        row.parent = parent;
        if (parent) {
            row.color = Color::byParent();
            row.linetype = kLinetypeByParent;
            row.lineweight = LineWeight::byParent();
        }
        auto id = tx.addLayer(std::move(row));
        if (!id)
            return makeError(id.error().code, "layer \"" + soFar + "\": " + id.error().message);
        if (created)
            created->push_back(soFar);
        parent = id.value();
    }
    return parent;
}

// ---- Properties -------------------------------------------------------------------------------

Json colorToJson(const Color& c) {
    switch (c.kind) {
        case ColorKind::ByLayer:
            return "ByLayer";
        case ColorKind::ByBlock:
            return "ByBlock";
        case ColorKind::ByParent:
            return "ByParent";
        case ColorKind::Index:
            return static_cast<int>(c.indexValue());
        case ColorKind::Rgb: {
            char buf[8];
            std::snprintf(buf, sizeof buf, "#%02X%02X%02X", c.r, c.g, c.b);
            return std::string(buf);
        }
    }
    return "ByLayer";
}

Result<Color> colorFromJson(const Json& j, bool forLayer) {
    if (j.isNumber()) {
        double d = j.asNumber();
        if (d < 1 || d > 255 || d != std::floor(d))
            return makeError(ErrorCode::InvalidArgument, "color index must be 1..255");
        return Color::index(static_cast<std::uint8_t>(d));
    }
    if (j.isString()) {
        std::string s = j.asString();
        std::string l = lower(s);
        if (l == "bylayer" && !forLayer)
            return Color::byLayer();
        if (l == "byblock" && !forLayer)
            return Color::byBlock();
        if (l == "byparent" && forLayer)
            return Color::byParent();
        if (s.size() == 7 && s[0] == '#') {
            unsigned v = 0;
            if (std::sscanf(s.c_str() + 1, "%6x", &v) == 1) {
                return Color::rgb(static_cast<std::uint8_t>(v >> 16),
                                  static_cast<std::uint8_t>(v >> 8), static_cast<std::uint8_t>(v));
            }
        }
    }
    return makeError(ErrorCode::InvalidArgument,
                     forLayer ? "color must be an ACI number 1..255, \"#RRGGBB\" or \"ByParent\""
                              : "color must be \"ByLayer\", \"ByBlock\", an ACI number 1..255 or "
                                "\"#RRGGBB\"");
}

Json lineweightToJson(const LineWeight& w) {
    if (w.isByLayer())
        return "ByLayer";
    if (w.isByBlock())
        return "ByBlock";
    if (w.isDefault())
        return "Default";
    if (w.isByParent())
        return "ByParent";
    return w.toMm();
}

Result<LineWeight> lineweightFromJson(const Json& j, bool forLayer) {
    if (j.isNumber()) {
        if (j.asNumber() < 0 || j.asNumber() > 2.11)
            return makeError(ErrorCode::InvalidArgument, "lineweight must be 0..2.11 mm");
        return LineWeight::mm(j.asNumber());
    }
    if (j.isString()) {
        std::string l = lower(j.asString());
        if (l == "default")
            return LineWeight::defaultWeight();
        if (l == "bylayer" && !forLayer)
            return LineWeight::byLayer();
        if (l == "byblock" && !forLayer)
            return LineWeight::byBlock();
        if (l == "byparent" && forLayer)
            return LineWeight::byParent();
    }
    return makeError(ErrorCode::InvalidArgument,
                     forLayer ? "lineweight must be a number (mm), \"Default\" or \"ByParent\""
                              : "lineweight must be a number (mm), \"ByLayer\", \"ByBlock\" or "
                                "\"Default\"");
}

Json linetypeToJson(const Document& doc, LinetypeId id) {
    if (id == kLinetypeByLayer)
        return "ByLayer";
    if (id == kLinetypeByBlock)
        return "ByBlock";
    if (id == kLinetypeByParent)
        return "ByParent";
    const Linetype* lt = doc.linetype(id);
    return lt ? lt->name : std::string("?");
}

Result<LinetypeId> linetypeFromJson(const Document& doc, const Json& j, bool forLayer) {
    if (!j.isString())
        return makeError(ErrorCode::InvalidArgument, "linetype must be a name");
    std::string l = lower(j.asString());
    if (l == "bylayer" && !forLayer)
        return kLinetypeByLayer;
    if (l == "byblock" && !forLayer)
        return kLinetypeByBlock;
    if (l == "byparent" && forLayer)
        return kLinetypeByParent;
    if (const Linetype* lt = doc.linetypeByName(j.asString()))
        return lt->id;
    return makeError(ErrorCode::NotFound, "unknown linetype \"" + j.asString() + "\"");
}

// ---- Entity -> JSON ---------------------------------------------------------------------------

std::string kindName(EntityKind kind) {
    switch (kind) {
        case EntityKind::Line:
            return "line";
        case EntityKind::Circle:
            return "circle";
        case EntityKind::Arc:
            return "arc";
        case EntityKind::Polyline:
            return "polyline";
        case EntityKind::Point:
            return "point";
        case EntityKind::Ellipse:
            return "ellipse";
        case EntityKind::Spline:
            return "spline";
        case EntityKind::Text:
            return "text";
        case EntityKind::MText:
            return "mtext";
        case EntityKind::Hatch:
            return "hatch";
        case EntityKind::Insert:
            return "insert";
        case EntityKind::Dimension:
            return "dimension";
        case EntityKind::Leader:
            return "leader";
        case EntityKind::Proxy:
            return "proxy";
    }
    return "proxy";
}

namespace {
std::string styleName(const Document& doc, TextStyleId id) {
    const TextStyle* s = id ? doc.textStyle(id) : nullptr;
    return s ? s->name : std::string();
}
}  // namespace

Json entityToJson(const Document& doc, const Entity& e) {
    Json::Object o;
    o["id"] = std::to_string(e.common.id.value);
    o["kind"] = kindName(kind(e));
    o["layer"] = layerPath(doc, e.common.layer);
    o["color"] = colorToJson(e.common.color);
    o["linetype"] = linetypeToJson(doc, e.common.linetype);
    o["lineweight"] = lineweightToJson(e.common.lineweight);
    if (e.common.linetypeScale != 1)
        o["linetypeScale"] = e.common.linetypeScale;
    if (!e.common.visible)
        o["visible"] = false;

    visitEntity(e, [&](const auto& d) {
        using T = std::decay_t<decltype(d)>;
        if constexpr (std::is_same_v<T, LineData>) {
            o["start"] = pointToJson(d.a);
            o["end"] = pointToJson(d.b);
        } else if constexpr (std::is_same_v<T, CircleData>) {
            o["center"] = pointToJson(d.center);
            o["radius"] = d.radius;
        } else if constexpr (std::is_same_v<T, ArcData>) {
            o["center"] = pointToJson(d.center);
            o["radius"] = d.radius;
            o["startAngle"] = d.startAngle;
            o["endAngle"] = d.endAngle;
        } else if constexpr (std::is_same_v<T, PolylineData>) {
            o["vertices"] = verticesToJson(d.v);
            o["closed"] = d.closed;
            if (d.constantWidth != 0)
                o["constantWidth"] = d.constantWidth;
        } else if constexpr (std::is_same_v<T, PointData>) {
            o["position"] = pointToJson(d.p);
        } else if constexpr (std::is_same_v<T, EllipseData>) {
            o["center"] = pointToJson(d.center);
            o["majorAxis"] = pointToJson(d.majorAxis);
            o["ratio"] = d.ratio;
            if (d.startParam != 0 || d.endParam != kTwoPi) {
                o["startParam"] = d.startParam;
                o["endParam"] = d.endParam;
            }
        } else if constexpr (std::is_same_v<T, SplineData>) {
            Json s = splineToJson(d);
            for (const auto& [k, v] : s.asObject())
                o[k] = v;
        } else if constexpr (std::is_same_v<T, TextData>) {
            o["position"] = pointToJson(d.position);
            o["height"] = d.height;
            o["value"] = d.utf8;
            if (d.rotation != 0)
                o["rotation"] = d.rotation;
            if (d.style)
                o["style"] = styleName(doc, d.style);
            o["hAlign"] = hAlignName(d.h);
            o["vAlign"] = vAlignName(d.v);
            if (d.h == HAlign::Aligned || d.h == HAlign::Fit)
                o["alignPoint"] = pointToJson(d.alignPoint);
            if (d.widthFactor != 1)
                o["widthFactor"] = d.widthFactor;
            if (d.oblique != 0)
                o["oblique"] = d.oblique;
        } else if constexpr (std::is_same_v<T, MTextData>) {
            o["position"] = pointToJson(d.position);
            o["height"] = d.height;
            if (d.width != 0)
                o["width"] = d.width;
            o["value"] = d.utf8;
            o["attachment"] = kAttachNames[static_cast<int>(d.attach)];
            if (d.rotation != 0)
                o["rotation"] = d.rotation;
            if (d.style)
                o["style"] = styleName(doc, d.style);
        } else if constexpr (std::is_same_v<T, HatchData>) {
            Json::Array loops;
            for (const auto& l : d.loops) {
                Json::Object lo;
                lo["outer"] = l.outer;
                Json::Array edges;
                for (const auto& edge : l.edges)
                    edges.push_back(edgeToJson(edge));
                lo["edges"] = Json(std::move(edges));
                loops.push_back(Json(std::move(lo)));
            }
            o["loops"] = Json(std::move(loops));
            o["solid"] = d.solid;
            if (!d.solid) {
                o["pattern"] = d.pattern;
                o["scale"] = d.patternScale;
                o["angle"] = d.patternAngle;
            }
        } else if constexpr (std::is_same_v<T, InsertData>) {
            const BlockDef* b = doc.block(d.block);
            o["block"] = b ? b->name : std::string("?");
            o["position"] = pointToJson(d.position);
            o["scale"] = pointToJson(d.scale);
            o["rotation"] = d.rotation;
        } else if constexpr (std::is_same_v<T, DimensionData>) {
            o["dimKind"] = dimKindName(d.kind);
            Json::Array pts;
            for (const auto& p : d.defPts)
                pts.push_back(pointToJson(p));
            o["definitionPoints"] = Json(std::move(pts));
            o["textPosition"] = pointToJson(d.textPos);
            o["measurement"] = d.measurement;
            if (!d.textOverride.empty())
                o["text"] = d.textOverride;
            if (d.kind == DimKind::Linear)
                o["rotation"] = d.rotation;
        } else if constexpr (std::is_same_v<T, LeaderData>) {
            o["vertices"] = pointsToJson(d.vertices);
            if (!d.utf8.empty())
                o["text"] = d.utf8;
            o["textPosition"] = pointToJson(d.textPos);
        } else if constexpr (std::is_same_v<T, ProxyData>) {
            o["extra"] = "unknown entity type \"" + d.originalType + "\", read-only";
        }
    });
    return Json(std::move(o));
}

// ---- JSON -> Entity ---------------------------------------------------------------------------

Result<Entity> entityFromJson(const Document& doc, const Json& j, const EntityParseOptions& opt) {
    try {
        if (!j.isObject())
            fail("an entity must be an object");
        std::string k = lower(strOr(j, "kind"));
        if (k.empty())
            fail("missing \"kind\"");
        Entity e;
        // Common properties.
        std::string layer = strOr(j, "layer", opt.defaultLayer);
        if (!layer.empty()) {
            const Layer* l = findLayerByPath(doc, layer);
            if (!l)
                fail("unknown layer \"" + layer +
                     "\" (create it first, or pass create_layers: true)");
            e.common.layer = l->id;
        }
        if (const Json* c = j.find("color"))
            e.common.color = orFail(colorFromJson(*c, false));
        if (const Json* c = j.find("linetype"))
            e.common.linetype = orFail(linetypeFromJson(doc, *c, false));
        if (const Json* c = j.find("lineweight"))
            e.common.lineweight = orFail(lineweightFromJson(*c, false));
        if (const Json* c = j.find("linetypeScale"))
            e.common.linetypeScale = c->asNumber(1);
        if (const Json* c = j.find("visible"))
            e.common.visible = c->asBool(true);

        if (k == "line") {
            e.data = LineData{pt(j, "start"), pt(j, "end")};
        } else if (k == "circle") {
            double r = lenField(j, "radius");
            if (!(r > 0))
                fail("radius must be positive");
            e.data = CircleData{pt(j, "center"), r};
        } else if (k == "arc") {
            double r = lenField(j, "radius");
            if (!(r > 0))
                fail("radius must be positive");
            double a0 = normalizeAngle(angle(req(j, "startAngle"), "startAngle"));
            double a1 = normalizeAngle(angle(req(j, "endAngle"), "endAngle"));
            if (tol::nearlyEqualAngle(a0, a1))
                fail("an arc needs different start and end angles (use a circle for 360 degrees)");
            e.data = ArcData{pt(j, "center"), r, a0, a1};
        } else if (k == "polyline") {
            PolylineData p;
            p.v = verticesFrom(req(j, "vertices"));
            if (p.v.size() < 2)
                fail("a polyline needs at least 2 vertices");
            p.closed = boolOr(j, "closed", false);
            p.constantWidth = lenOr(j, "constantWidth", 0);
            e.data = std::move(p);
        } else if (k == "point") {
            e.data = PointData{pt(j, j.find("position") ? "position" : "point")};
        } else if (k == "ellipse") {
            EllipseData el;
            el.center = pt(j, "center");
            el.majorAxis = pt(j, "majorAxis");
            el.ratio = j.find("ratio") ? j.find("ratio")->asNumber(1) : 1;
            if (!(el.ratio > 0 && el.ratio <= 1))
                fail("ratio (minor/major) must be in (0, 1]");
            if (length(el.majorAxis) <= 0)
                fail("majorAxis must not be zero");
            el.startParam = angOr(j, "startParam", 0);
            el.endParam = angOr(j, "endParam", kTwoPi);
            e.data = Indirect<EllipseData>(std::move(el));
        } else if (k == "spline") {
            e.data = Indirect<SplineData>(splineFromJson(j));
        } else if (k == "text") {
            TextData t;
            t.position = pt(j, "position");
            t.height = lenOr(j, "height", 2.5);
            if (!(t.height > 0))
                fail("height must be positive");
            t.utf8 = strOr(j, "value");
            if (!j.find("value") || !j.find("value")->isString())
                fail("missing \"value\"");
            t.rotation = angOr(j, "rotation", 0);
            t.widthFactor = j.find("widthFactor") ? j.find("widthFactor")->asNumber(1) : 1;
            t.oblique = angOr(j, "oblique", 0);
            std::string st = strOr(j, "style");
            if (!st.empty()) {
                const TextStyle* s = doc.textStyleByName(st);
                if (!s)
                    fail("unknown text style \"" + st + "\"");
                t.style = s->id;
            }
            std::string h = lower(strOr(j, "hAlign", "left")),
                        v = lower(strOr(j, "vAlign", "baseline"));
            const char* hn[] = {"left", "center", "right", "aligned", "middle", "fit"};
            const char* vn[] = {"baseline", "bottom", "middle", "top"};
            bool found = false;
            for (int i = 0; i < 6; ++i) {
                if (h == hn[i]) {
                    t.h = static_cast<HAlign>(i);
                    found = true;
                }
            }
            if (!found)
                fail("hAlign must be left, center, right, aligned, middle or fit");
            found = false;
            for (int i = 0; i < 4; ++i) {
                if (v == vn[i]) {
                    t.v = static_cast<VAlign>(i);
                    found = true;
                }
            }
            if (!found)
                fail("vAlign must be baseline, bottom, middle or top");
            if (j.find("alignPoint"))
                t.alignPoint = pt(j, "alignPoint");
            e.data = Indirect<TextData>(std::move(t));
        } else if (k == "mtext") {
            MTextData t;
            t.position = pt(j, "position");
            t.height = lenOr(j, "height", 2.5);
            if (!(t.height > 0))
                fail("height must be positive");
            t.width = lenOr(j, "width", 0);
            if (!j.find("value") || !j.find("value")->isString())
                fail("missing \"value\"");
            t.utf8 = strOr(j, "value");
            t.rotation = angOr(j, "rotation", 0);
            std::string a = strOr(j, "attachment", "topLeft");
            bool found = false;
            for (int i = 0; i < 9; ++i) {
                if (lower(a) == lower(kAttachNames[i])) {
                    t.attach = static_cast<Attachment>(i);
                    found = true;
                }
            }
            if (!found)
                fail(
                    "attachment must be one of topLeft, topCenter, topRight, middleLeft, "
                    "middleCenter, middleRight, bottomLeft, bottomCenter, bottomRight");
            std::string st = strOr(j, "style");
            if (!st.empty()) {
                const TextStyle* s = doc.textStyleByName(st);
                if (!s)
                    fail("unknown text style \"" + st + "\"");
                t.style = s->id;
            }
            e.data = Indirect<MTextData>(std::move(t));
        } else if (k == "hatch") {
            HatchData h;
            const Json& loops = req(j, "loops");
            if (!loops.isArray() || loops.asArray().empty())
                fail("a hatch needs \"loops\"");
            for (const auto& l : loops.asArray())
                h.loops.push_back(loopFromJson(l));
            h.pattern = strOr(j, "pattern");
            h.solid = h.pattern.empty() || lower(h.pattern) == "solid";
            if (h.solid)
                h.pattern.clear();
            h.patternScale = j.find("scale") ? j.find("scale")->asNumber(1) : 1;
            h.patternAngle = angOr(j, "angle", 0);
            if (!(h.patternScale > 0))
                fail("scale must be positive");
            e.data = Indirect<HatchData>(std::move(h));
        } else if (k == "insert") {
            InsertData in;
            std::string bn = strOr(j, "block");
            const BlockDef* b = doc.blockByName(bn);
            if (!b)
                fail("unknown block \"" + bn + "\"");
            in.block = b->id;
            in.position = pt(j, "position");
            if (const Json* s = j.find("scale")) {
                if (s->isNumber())
                    in.scale = {s->asNumber(), s->asNumber()};
                else
                    in.scale = orFail(pointFromJson(*s));
            }
            in.rotation = angOr(j, "rotation", 0);
            e.data = Indirect<InsertData>(std::move(in));
        } else if (k == "dimension" || k == "leader") {
            fail(
                "dimensions and leaders are created with run_command (DIMLINEAR, DIMALIGNED, "
                "DIMRADIUS, ... and LEADER), which own their geometry rules");
        } else if (k == "proxy") {
            fail("proxy entities are read-only");
        } else {
            fail("unknown kind \"" + k + "\"");
        }
        return e;
    } catch (const ParseError& err) {
        return makeError(ErrorCode::InvalidArgument, err.what());
    }
}

}  // namespace tbcad::mcp
