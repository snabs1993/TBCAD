#include <cmath>
#include <set>

#include "tools_common.h"

namespace tbcad::mcp::detail {

namespace {

const std::string kAddLabel = std::string(kAiLabelPrefix) + "add_entities";
const std::string kModifyLabel = std::string(kAiLabelPrefix) + "modify_entities";
const std::string kDeleteLabel = std::string(kAiLabelPrefix) + "delete_entities";

void commit(DrawingSession& s, Transaction& tx, const std::string& label) {
    ChangeSet cs = tx.commit();
    s.undoStack().push(label, {std::move(cs)});
}

ToolResult addEntities(ToolContext& ctx, const Json& a) {
    DrawingSession& s = ctx.session;
    Document& doc = s.document();
    const auto& list = a.find("entities")->asArray();
    std::string defLayer = argString(a, "layer");
    bool create = argBool(a, "create_layers");

    auto tx = doc.begin(kAddLabel);
    std::set<std::string> wanted;
    if (!defLayer.empty())
        wanted.insert(defLayer);
    for (const auto& e : list) {
        if (const Json* l = e.find("layer"); l && l->isString() && !l->asString().empty())
            wanted.insert(l->asString());
    }
    std::vector<std::string> createdLayers;
    for (const auto& path : wanted) {
        if (findLayerByPath(doc, path))
            continue;
        if (!create)
            return ToolResult::fail(
                "unknown layer \"" + path +
                "\". Create it with create_layer, or pass create_layers: true.");
        auto r = ensureLayerPath(doc, tx, path, &createdLayers);
        if (!r)
            return ToolResult::fail(r.error().message);
    }
    EntityParseOptions opt;
    opt.defaultLayer = defLayer;
    std::vector<EntityId> ids;
    for (std::size_t i = 0; i < list.size(); ++i) {
        auto e = entityFromJson(doc, list[i], opt);
        if (!e)
            return ToolResult::fail("entities[" + std::to_string(i) + "]: " + e.error().message);
        auto id = tx.add(std::move(e.value()));
        if (!id)
            return ToolResult::fail("entities[" + std::to_string(i) + "]: " + id.error().message);
        ids.push_back(id.value());
    }
    commit(s, tx, kAddLabel);
    Json::Array layers;
    for (auto& l : createdLayers)
        layers.emplace_back(l);
    return ToolResult::ok(Json(Json::Object{{"ids", idsJson(ids)},
                                            {"count", static_cast<double>(ids.size())},
                                            {"createdLayers", Json(std::move(layers))}}));
}

const std::set<std::string>& commonKeys() {
    static const std::set<std::string> k{"layer",      "color",         "linetype",
                                         "lineweight", "linetypeScale", "visible"};
    return k;
}
const std::set<std::string>& optionalGeometryKeys() {
    static const std::set<std::string> k{"rotation",      "style",      "alignPoint", "widthFactor",
                                         "oblique",       "width",      "startParam", "endParam",
                                         "constantWidth", "weights",    "fitPoints",  "hAlign",
                                         "vAlign",        "attachment", "closed",     "ratio"};
    return k;
}

// Applies `set` to e. Returns an error text or empty.
std::string applySet(const Document& doc, Entity& e, const Json& set) {
    for (const auto& [key, v] : set.asObject()) {
        if (key == "id")
            return "the id cannot be changed";
        if (key == "kind" && v.asString() != kindName(kind(e)))
            return "the kind of an entity cannot be changed";
    }
    // Common properties.
    if (const Json* v = set.find("layer")) {
        const Layer* l = v->isString() ? findLayerByPath(doc, v->asString()) : nullptr;
        if (!l)
            return "unknown layer " + dump(*v) + " (create it first with create_layer)";
        e.common.layer = l->id;
    }
    if (const Json* v = set.find("color")) {
        auto r = colorFromJson(*v, false);
        if (!r)
            return r.error().message;
        e.common.color = r.value();
    }
    if (const Json* v = set.find("linetype")) {
        auto r = linetypeFromJson(doc, *v, false);
        if (!r)
            return r.error().message;
        e.common.linetype = r.value();
    }
    if (const Json* v = set.find("lineweight")) {
        auto r = lineweightFromJson(*v, false);
        if (!r)
            return r.error().message;
        e.common.lineweight = r.value();
    }
    if (const Json* v = set.find("linetypeScale"))
        e.common.linetypeScale = v->asNumber(1);
    if (const Json* v = set.find("visible"))
        e.common.visible = v->asBool(true);

    bool geometry = false;
    for (const auto& [key, v] : set.asObject()) {
        if (key != "id" && key != "kind" && !commonKeys().count(key))
            geometry = true;
    }
    if (!geometry)
        return {};

    if (auto* h = entityAs<HatchData>(e)) {
        for (const auto& [key, v] : set.asObject()) {
            if (commonKeys().count(key) || key == "id" || key == "kind")
                continue;
            if (key == "pattern" && v.isString()) {
                h->pattern = v.asString();
                h->solid = h->pattern.empty() || h->pattern == "solid";
                if (h->solid)
                    h->pattern.clear();
            } else if (key == "scale" && v.isNumber() && v.asNumber() > 0) {
                h->patternScale = v.asNumber();
            } else if (key == "angle") {
                auto r = angleFromJson(v);
                if (!r)
                    return r.error().message;
                h->patternAngle = r.value();
            } else {
                return "a hatch accepts pattern, scale, angle and the common properties";
            }
        }
        return {};
    }
    EntityKind k = kind(e);
    if (k == EntityKind::Dimension || k == EntityKind::Leader || k == EntityKind::Proxy) {
        return "the geometry of a " + kindName(k) +
               " is changed with transform or run_command, not with set";
    }
    Json merged = entityToJson(doc, e);
    for (const auto& [key, v] : set.asObject()) {
        if (commonKeys().count(key) || key == "id" || key == "kind")
            continue;
        if (!merged.find(key) && !optionalGeometryKeys().count(key)) {
            std::string known;
            for (const auto& [mk, mv] : merged.asObject()) {
                if (mk != "id" && !commonKeys().count(mk) && mk != "kind")
                    known += (known.empty() ? "" : ", ") + mk;
            }
            return "unknown property \"" + key + "\" for a " + kindName(k) + " (known: " + known +
                   ")";
        }
        merged.object()[key] = v;
    }
    EntityParseOptions opt;
    auto parsed = entityFromJson(doc, merged, opt);
    if (!parsed)
        return parsed.error().message;
    e.data = parsed.value().data;
    return {};
}

Result<Transform2> parseTransform(const Json& t) {
    int given = 0;
    for (const char* k : {"move", "rotate", "scale", "mirror"})
        given += t.find(k) ? 1 : 0;
    if (given != 1)
        return makeError(ErrorCode::InvalidArgument,
                         "transform needs exactly one of move, rotate, scale, mirror");
    if (const Json* m = t.find("move")) {
        auto d = pointFromJson(*m);
        if (!d)
            return makeError(ErrorCode::InvalidArgument, "move must be [dx, dy]");
        return Transform2::translate(d.value());
    }
    if (const Json* r = t.find("rotate")) {
        const Json* base = r->find("base");
        const Json* ang = r->find("angle");
        if (!base || !ang)
            return makeError(ErrorCode::InvalidArgument, "rotate needs base and angle");
        auto b = pointFromJson(*base);
        auto an = angleFromJson(*ang);
        if (!b || !an)
            return makeError(ErrorCode::InvalidArgument, "rotate needs base [x, y] and an angle");
        return Transform2::rotate(an.value(), b.value());
    }
    if (const Json* sc = t.find("scale")) {
        const Json* base = sc->find("base");
        const Json* f = sc->find("factor");
        if (!base || !f || !f->isNumber())
            return makeError(ErrorCode::InvalidArgument, "scale needs base and a numeric factor");
        auto b = pointFromJson(*base);
        if (!b)
            return makeError(ErrorCode::InvalidArgument, "scale base must be [x, y]");
        if (!(f->asNumber() > 0))
            return makeError(ErrorCode::InvalidArgument, "scale factor must be positive");
        return Transform2::scale(f->asNumber(), f->asNumber(), b.value());
    }
    const Json* mi = t.find("mirror");
    const Json* p1 = mi->find("p1");
    const Json* p2 = mi->find("p2");
    if (!p1 || !p2)
        return makeError(ErrorCode::InvalidArgument, "mirror needs p1 and p2");
    auto a = pointFromJson(*p1);
    auto b = pointFromJson(*p2);
    if (!a || !b)
        return makeError(ErrorCode::InvalidArgument, "mirror points must be [x, y]");
    return Transform2::mirror(a.value(), b.value());
}

ToolResult modifyEntities(ToolContext& ctx, const Json& a) {
    DrawingSession& s = ctx.session;
    Document& doc = s.document();
    auto ids = parseIds(doc, a.find("ids"));
    if (!ids)
        return ToolResult::fail(ids.error().message);
    const Json* set = a.find("set");
    const Json* transform = a.find("transform");
    if (!set && !transform)
        return ToolResult::fail("nothing to do: pass \"set\" and/or \"transform\"");
    std::optional<Transform2> t;
    if (transform) {
        auto r = parseTransform(*transform);
        if (!r)
            return ToolResult::fail(r.error().message);
        t = r.value();
    }
    auto tx = doc.begin(kModifyLabel);
    for (EntityId id : ids.value()) {
        Entity e = *doc.find(id);
        if (set) {
            std::string err = applySet(doc, e, *set);
            if (!err.empty())
                return ToolResult::fail("entity " + std::to_string(id.value) + ": " + err);
        }
        if (t) {
            auto r = geom::transformed(e, *t);
            if (!r) {
                return ToolResult::fail("entity " + std::to_string(id.value) + " (" +
                                        kindName(kind(e)) + "): " + r.error().message +
                                        ". Try run_command with MOVE, ROTATE, SCALE or MIRROR.");
            }
            e.data = r.value().data;
        }
        auto m = tx.modify(id, [&](Entity& x) {
            x.common = e.common;
            x.common.id = id;
            x.data = e.data;
        });
        if (!m)
            return ToolResult::fail("entity " + std::to_string(id.value) + ": " +
                                    m.error().message);
    }
    commit(s, tx, kModifyLabel);
    return ToolResult::ok(
        Json(Json::Object{{"modified", static_cast<double>(ids.value().size())}}));
}

ToolResult deleteEntities(ToolContext& ctx, const Json& a) {
    DrawingSession& s = ctx.session;
    Document& doc = s.document();
    auto ids = parseIds(doc, a.find("ids"));
    if (!ids)
        return ToolResult::fail(ids.error().message);
    std::set<EntityId> unique(ids.value().begin(), ids.value().end());
    auto tx = doc.begin(kDeleteLabel);
    for (EntityId id : unique) {
        auto r = tx.erase(id);
        if (!r)
            return ToolResult::fail("entity " + std::to_string(id.value) + ": " +
                                    r.error().message);
    }
    commit(s, tx, kDeleteLabel);
    // Drop erased entities from the selection.
    auto sel = s.selection();
    sel.erase(
        std::remove_if(sel.begin(), sel.end(), [&](EntityId id) { return unique.count(id) > 0; }),
        sel.end());
    s.setSelection(std::move(sel));
    return ToolResult::ok(Json(Json::Object{{"deleted", static_cast<double>(unique.size())}}));
}

ToolResult setSelection(ToolContext& ctx, const Json& a) {
    auto ids = parseIds(ctx.session.document(), a.find("ids"));
    if (!ids)
        return ToolResult::fail(ids.error().message);
    ctx.session.setSelection(ids.value());
    return ToolResult::ok(Json(Json::Object{{"count", static_cast<double>(ids.value().size())}}));
}

}  // namespace

void registerEditTools(ToolSet& tools) {
    auto idArray = [](const char* d = "Entity ids (decimal strings)") {
        return tArray(tString(), d);
    };

    Json entitySchema = tObject(
        {{"kind", tString("line, circle, arc, polyline, point, ellipse, spline, text, mtext, hatch "
                          "or insert")},
         {"layer", tString("layer path; default: the `layer` argument or the current layer")},
         {"color", tAny("\"ByLayer\", \"ByBlock\", ACI 1..255 or \"#RRGGBB\"")},
         {"linetype", tString("name, \"ByLayer\" or \"ByBlock\"")},
         {"lineweight", tAny("mm, \"ByLayer\", \"ByBlock\" or \"Default\"")}},
        {"kind"},
        "Entity JSON as returned by query_entities (without id). Points are [x, y] in mm; angles "
        "are radians or {\"deg\": n}; lengths may be {\"value\": 3, \"unit\": \"m\"}. line: "
        "start,end. circle: center,radius. arc: center,radius,startAngle,endAngle (counter-"
        "clockwise). polyline: vertices ([[x,y]..] or [{x,y,bulge}..]), closed. point: position. "
        "ellipse: center,majorAxis,ratio. spline: degree,controlPoints. text: "
        "position,height,value. mtext: position,height,width,value. hatch: loops "
        "([[[x,y]..]..]), pattern,scale,angle. insert: block,position,scale,rotation.");

    tools.add({"add_entities", "Add entities",
               "Adds geometry to the drawing in one undo step. Dimensions and leaders are made "
               "with run_command.",
               tObject({{"entities", tArray(entitySchema, "", 10000)},
                        {"layer", tString("default layer path for entities without one")},
                        {"create_layers", tBool("create layers (and parents) that do not exist")}},
                       {"entities"}),
               outSchema({"ids", "count", "createdLayers"}), Edit::Document, false, false, true,
               addEntities});

    tools.add({"modify_entities", "Modify entities",
               "Changes properties and/or geometry of entities, or moves/rotates/scales/mirrors "
               "them, in one undo step. `set` takes the same fields as the entity JSON "
               "(e.g. {\"layer\": \"A-WALL\", \"color\": 1, \"radius\": 50}). `transform` is one "
               "of {move:[dx,dy]}, {rotate:{base,angle}}, {scale:{base,factor}}, "
               "{mirror:{p1,p2}}.",
               tObject({{"ids", idArray()},
                        {"set", tObject({})},
                        {"transform",
                         tObject({{"move", tPoint("[dx, dy]")},
                                  {"rotate", tObject({{"base", tPoint()}, {"angle", tAny()}})},
                                  {"scale", tObject({{"base", tPoint()}, {"factor", tNumber()}})},
                                  {"mirror", tObject({{"p1", tPoint()}, {"p2", tPoint()}})}})}},
                       {"ids"}),
               outSchema({"modified"}), Edit::Document, false, false, true, modifyEntities});

    tools.add({"delete_entities", "Delete entities", "Erases entities in one undo step.",
               tObject({{"ids", idArray()}}, {"ids"}), outSchema({"deleted"}), Edit::Document, true,
               false, true, deleteEntities});

    tools.add({"set_selection", "Set selection",
               "Selects entities in TBCAD (highlights them for the user). Not an undo step.",
               tObject({{"ids", idArray()}}, {"ids"}), outSchema({"count"}), Edit::Session, false,
               true, true, setSelection});
}

}  // namespace tbcad::mcp::detail
