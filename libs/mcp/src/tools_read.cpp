#include <cmath>

#include "tools_common.h"

namespace tbcad::mcp::detail {

std::vector<EntityId> modelSpaceIds(const Document& doc) {
    std::vector<EntityId> ids;
    ids.reserve(doc.modelSpaceCount());
    doc.forEachEntity([&](const Entity& e) {
        if (!doc.ownerBlock(e.common.id))
            ids.push_back(e.common.id);
    });
    std::sort(ids.begin(), ids.end());
    return ids;
}

BBox2 drawingExtents(DrawingSession& s) {
    if (const auto* idx = s.spatialIndex())
        return idx->bounds();
    BBox2 box;
    const Document& doc = s.document();
    doc.forEachEntity([&](const Entity& e) {
        if (!doc.ownerBlock(e.common.id))
            box.merge(geom::bounds(e, doc));
    });
    return box;
}

Json bboxJson(const BBox2& b) {
    if (b.empty())
        return Json{};
    return Json(Json::Object{{"min", pointToJson(b.min)}, {"max", pointToJson(b.max)}});
}

void ChangeRecorder::onChanged(const ChangeSet& cs) {
    for (const auto& c : cs.entities) {
        EntityId id = cs.idOf(c);
        if (c.before == kNoSnapshot) {
            state_[id] = 1;
        } else if (c.after == kNoSnapshot) {
            auto it = state_.find(id);
            if (it != state_.end() && it->second == 1)
                state_.erase(it);  // created and erased again: no net change
            else
                state_[id] = 3;
        } else if (!state_.count(id)) {
            state_[id] = 2;
        }
    }
}

std::vector<EntityId> ChangeRecorder::pick(int what) const {
    std::vector<EntityId> out;
    for (const auto& [id, st] : state_) {
        if (st == what)
            out.push_back(id);
    }
    return out;
}

namespace {

Json layerNode(const Document& doc, const Layer& l, bool eff,
               const std::map<LayerId, std::size_t>& counts,
               const std::multimap<LayerId, const Layer*>& children) {
    Json::Object o;
    o["name"] = l.name;
    o["path"] = layerPath(doc, l.id);
    if (l.parent)
        o["parent"] = layerPath(doc, l.parent);
    o["color"] = colorToJson(l.color);
    o["linetype"] = linetypeToJson(doc, l.linetype);
    o["lineweight"] = lineweightToJson(l.lineweight);
    o["on"] = l.on;
    o["frozen"] = l.frozen;
    o["locked"] = l.locked;
    o["plot"] = l.plot;
    auto it = counts.find(l.id);
    o["entityCount"] = static_cast<double>(it == counts.end() ? 0 : it->second);
    if (eff) {
        EffectiveLayer e = doc.effective(l.id);
        o["effective"] = Json(Json::Object{{"color", colorToJson(e.color)},
                                           {"linetype", linetypeToJson(doc, e.linetype)},
                                           {"lineweight", lineweightToJson(e.lineweight)},
                                           {"on", e.on},
                                           {"frozen", e.frozen},
                                           {"locked", e.locked},
                                           {"plot", e.plot}});
    }
    Json::Array kids;
    auto range = children.equal_range(l.id);
    for (auto k = range.first; k != range.second; ++k)
        kids.push_back(layerNode(doc, *k->second, eff, counts, children));
    o["children"] = Json(std::move(kids));
    return Json(std::move(o));
}

}  // namespace

Json layersJson(const Document& doc, bool includeEffective) {
    std::map<LayerId, std::size_t> counts;
    doc.forEachEntity([&](const Entity& e) {
        if (!doc.ownerBlock(e.common.id))
            ++counts[e.common.layer];
    });
    std::multimap<LayerId, const Layer*> children;
    for (const auto& [id, l] : doc.layers())
        children.emplace(l.parent, &l);
    Json::Array roots;
    auto range = children.equal_range(LayerId{});
    for (auto k = range.first; k != range.second; ++k)
        roots.push_back(layerNode(doc, *k->second, includeEffective, counts, children));
    return Json(Json::Object{{"layers", Json(std::move(roots))},
                             {"currentLayer", layerPath(doc, doc.header().currentLayer)},
                             {"layerCount", static_cast<double>(doc.layers().size())}});
}

Json drawingInfoJson(DrawingSession& s, const std::string& serverVersion) {
    const Document& doc = s.document();
    FileInfo f = s.file();
    Json::Object o;
    o["name"] = f.name;
    o["path"] = f.hasFile ? Json(f.path.generic_string()) : Json{};
    o["modified"] = f.modified;
    o["units"] = "mm";
    o["angles"] = "radians";
    o["revision"] = static_cast<double>(doc.revision());
    o["entityCount"] = static_cast<double>(doc.modelSpaceCount());
    o["extents"] = bboxJson(drawingExtents(s));
    o["currentLayer"] = layerPath(doc, doc.header().currentLayer);
    o["layerCount"] = static_cast<double>(doc.layers().size());
    o["blockCount"] = static_cast<double>(doc.blocks().size());
    o["undoLabel"] = s.undoStack().undoLabel();
    o["redoLabel"] = s.undoStack().redoLabel();
    bool running = s.runner().active();
    o["commandRunning"] = running;
    if (running && s.runner().prompt())
        o["commandPrompt"] = s.runner().prompt()->display();
    o["selectionCount"] = static_cast<double>(s.selection().size());
    if (auto v = s.view()) {
        o["view"] = Json(Json::Object{{"center", pointToJson(v->center)},
                                      {"mmPerPixel", v->mmPerPixel},
                                      {"widthPx", v->widthPx},
                                      {"heightPx", v->heightPx}});
    }
    o["serverVersion"] = serverVersion;
    return Json(std::move(o));
}

Json commandsJson(DrawingSession& s, const std::string& filter) {
    std::string f = cmd::toUpperAscii(filter);
    Json::Array list;
    for (cmd::Command* c : s.registry().list()) {
        if (c->description().empty())
            continue;  // hidden developer commands
        std::string name(c->name());
        std::string desc(c->description());
        auto aliases = s.registry().aliasesOf(name);
        if (!f.empty()) {
            bool hit = name.find(f) != std::string::npos ||
                       cmd::toUpperAscii(desc).find(f) != std::string::npos;
            for (const auto& a : aliases)
                hit = hit || a == f;
            if (!hit)
                continue;
        }
        Json::Array al;
        for (auto& a : aliases)
            al.emplace_back(a);
        list.push_back(Json(Json::Object{{"name", name},
                                         {"aliases", Json(std::move(al))},
                                         {"description", desc},
                                         {"transparent", c->isTransparent()}}));
    }
    return Json(Json::Object{{"commands", Json(std::move(list))}});
}

namespace {

ToolResult getDrawingInfo(ToolContext& ctx, const Json&) {
    return ToolResult::ok(drawingInfoJson(ctx.session, ctx.serverVersion));
}

ToolResult listLayers(ToolContext& ctx, const Json& a) {
    return ToolResult::ok(layersJson(ctx.session.document(), argBool(a, "include_effective", true)));
}

bool layerInSet(const Document& doc, LayerId id, const std::set<LayerId>& wanted, bool sub) {
    for (LayerId p = id; p; p = doc.layer(p) ? doc.layer(p)->parent : LayerId{}) {
        if (wanted.count(p))
            return true;
        if (!sub)
            return false;
    }
    return false;
}

ToolResult queryEntities(ToolContext& ctx, const Json& a) {
    DrawingSession& s = ctx.session;
    const Document& doc = s.document();

    std::vector<EntityId> candidates;
    std::optional<BBox2> box;
    if (const Json* b = a.find("bbox")) {
        if (!b->isArray() || b->asArray().size() != 2)
            return ToolResult::fail("bbox must be [[x1, y1], [x2, y2]]");
        auto p1 = pointFromJson(b->asArray()[0]);
        auto p2 = pointFromJson(b->asArray()[1]);
        if (!p1 || !p2)
            return ToolResult::fail("bbox must be [[x1, y1], [x2, y2]]");
        BBox2 bb;
        bb.expand(p1.value());
        bb.expand(p2.value());
        box = bb;
    }
    if (const Json* ids = a.find("ids")) {
        auto parsed = parseIds(doc, ids);
        if (!parsed)
            return ToolResult::fail(parsed.error().message);
        candidates = std::move(parsed.value());
        std::sort(candidates.begin(), candidates.end());
    } else if (box && s.spatialIndex()) {
        candidates = s.spatialIndex()->queryIds(*box);
        std::sort(candidates.begin(), candidates.end());
    } else {
        candidates = modelSpaceIds(doc);
    }

    std::set<LayerId> layers;
    bool filterLayers = false;
    if (const Json* l = a.find("layers")) {
        filterLayers = true;
        for (const auto& p : l->asArray()) {
            const Layer* layer = findLayerByPath(doc, p.asString());
            if (!layer)
                return ToolResult::fail("unknown layer \"" + p.asString() + "\"");
            layers.insert(layer->id);
        }
    }
    bool sub = argBool(a, "include_sublayers", true);
    std::set<std::string> kinds;
    if (const Json* k = a.find("kinds")) {
        for (const auto& v : k->asArray())
            kinds.insert(v.asString());
    }
    bool crossing = argString(a, "mode", "crossing") == "crossing";

    EntityId after{};
    if (const Json* c = a.find("cursor")) {
        auto id = parseId(*c);
        if (!id)
            return ToolResult::fail("bad cursor");
        after = *id;
    }
    auto limit = static_cast<std::size_t>(argNumber(a, "limit", 200));
    Json::Array out;
    std::size_t total = 0;
    std::string next;
    EntityId lastId = after;
    for (EntityId id : candidates) {
        const Entity* e = doc.find(id);
        if (!e)
            continue;
        if (filterLayers && !layerInSet(doc, e->common.layer, layers, sub))
            continue;
        if (!kinds.empty() && !kinds.count(kindName(kind(*e))))
            continue;
        if (box) {
            bool hit = crossing ? geom::crossesWindow(*e, *box, &doc)
                                : geom::insideWindow(*e, *box, &doc);
            if (!hit)
                continue;
        }
        ++total;
        if (id.value <= after.value)
            continue;
        if (out.size() < limit) {
            out.push_back(entityToJson(doc, *e));
            lastId = id;
        } else if (next.empty()) {
            next = std::to_string(lastId.value);
        }
    }
    Json::Object o;
    o["entities"] = Json(std::move(out));
    o["total"] = static_cast<double>(total);
    if (!next.empty())
        o["next_cursor"] = next;
    return ToolResult::ok(Json(std::move(o)));
}

ToolResult getEntity(ToolContext& ctx, const Json& a) {
    const Document& doc = ctx.session.document();
    auto id = parseId(*a.find("id"));
    if (!id || !doc.contains(*id) || doc.ownerBlock(*id))
        return ToolResult::fail("no such entity: " + dump(*a.find("id")));
    return ToolResult::ok(entityToJson(doc, *doc.find(*id)));
}

double entityArea(const Entity& e, std::optional<double>& perimeter) {
    perimeter.reset();
    if (const auto* pl = entityAs<PolylineData>(e)) {
        perimeter = geom::length(*pl);
        return geom::area(*pl);
    }
    if (const auto* c = entityAs<CircleData>(e)) {
        perimeter = kTwoPi * c->radius;
        return kPi * c->radius * c->radius;
    }
    if (const auto* el = entityAs<EllipseData>(e)) {
        if (std::abs((el->endParam - el->startParam) - kTwoPi) < 1e-9) {
            double aa = length(el->majorAxis), bb = aa * el->ratio;
            perimeter = geom::length(e);
            return kPi * aa * bb;
        }
    }
    return -1;
}

ToolResult measure(ToolContext& ctx, const Json& a) {
    const Document& doc = ctx.session.document();
    std::string what = argString(a, "kind");
    if (what == "distance") {
        const Json* pts = a.find("points");
        if (!pts || pts->asArray().size() != 2)
            return ToolResult::fail("distance needs points: [[x1, y1], [x2, y2]]");
        auto p1 = pointFromJson(pts->asArray()[0]);
        auto p2 = pointFromJson(pts->asArray()[1]);
        if (!p1 || !p2)
            return ToolResult::fail("distance needs points: [[x1, y1], [x2, y2]]");
        Vec2 d = p2.value() - p1.value();
        return ToolResult::ok(Json(Json::Object{
            {"distance", length(d)}, {"dx", d.x}, {"dy", d.y}, {"angle", std::atan2(d.y, d.x)}}));
    }
    if (what == "area" && a.find("points")) {
        auto pts = a.find("points")->asArray();
        if (pts.size() < 3)
            return ToolResult::fail("area needs at least 3 points");
        PolylineData pl;
        for (const auto& p : pts) {
            auto v = pointFromJson(p);
            if (!v)
                return ToolResult::fail("points must be [x, y] pairs");
            pl.v.push_back({v.value(), 0, 0, 0});
        }
        pl.closed = true;
        return ToolResult::ok(
            Json(Json::Object{{"area", geom::area(pl)}, {"perimeter", geom::length(pl)}}));
    }
    auto ids = parseIds(doc, a.find("ids"));
    if (!ids)
        return ToolResult::fail(ids.error().message);
    if (ids.value().empty())
        return ToolResult::fail("pass ids (or points) to measure");
    if (what == "length") {
        double total = 0;
        for (EntityId id : ids.value())
            total += geom::length(*doc.find(id));
        return ToolResult::ok(Json(Json::Object{{"length", total}}));
    }
    if (what == "area") {
        double total = 0, per = 0;
        for (EntityId id : ids.value()) {
            std::optional<double> p;
            double ar = entityArea(*doc.find(id), p);
            if (ar < 0)
                return ToolResult::fail("entity " + std::to_string(id.value) +
                                        " has no area (use a closed polyline, circle or ellipse)");
            total += ar;
            per += p.value_or(0);
        }
        return ToolResult::ok(Json(Json::Object{{"area", total}, {"perimeter", per}}));
    }
    return ToolResult::fail("kind must be distance, area or length");
}

ToolResult getSelection(ToolContext& ctx, const Json&) {
    const Document& doc = ctx.session.document();
    auto ids = ctx.session.selection();
    std::map<std::string, std::size_t> perKind;
    std::size_t count = 0;
    for (EntityId id : ids) {
        if (const Entity* e = doc.find(id)) {
            ++perKind[kindName(kind(*e))];
            ++count;
        }
    }
    Json::Object kinds;
    for (const auto& [k, n] : perKind)
        kinds[k] = static_cast<double>(n);
    bool cut = ids.size() > 2000;
    if (cut)
        ids.resize(2000);
    return ToolResult::ok(Json(Json::Object{{"count", static_cast<double>(count)},
                                            {"kinds", Json(std::move(kinds))},
                                            {"ids", idsJson(ids)},
                                            {"truncated", cut}}));
}

ToolResult listCommands(ToolContext& ctx, const Json& a) {
    return ToolResult::ok(commandsJson(ctx.session, argString(a, "filter")));
}

ToolResult listBlocks(ToolContext& ctx, const Json&) {
    const Document& doc = ctx.session.document();
    Json::Array rows;
    for (const auto& [id, b] : doc.blocks()) {
        if (b.anonymous)
            continue;
        rows.push_back(Json(Json::Object{{"name", b.name},
                                         {"base", pointToJson(b.base)},
                                         {"entityCount", static_cast<double>(b.entities.size())}}));
    }
    return ToolResult::ok(Json(Json::Object{{"blocks", Json(std::move(rows))}}));
}

ToolResult listTextStyles(ToolContext& ctx, const Json&) {
    const Document& doc = ctx.session.document();
    Json::Array rows;
    for (const auto& [id, s] : doc.textStyles()) {
        rows.push_back(Json(Json::Object{{"name", s.name},
                                         {"font", s.fontFamily},
                                         {"widthFactor", s.widthFactor},
                                         {"oblique", s.oblique}}));
    }
    return ToolResult::ok(Json(Json::Object{{"textStyles", Json(std::move(rows))}}));
}

ToolResult listLinetypes(ToolContext& ctx, const Json&) {
    const Document& doc = ctx.session.document();
    Json::Array rows;
    for (const auto& [id, l] : doc.linetypes()) {
        Json::Array pat;
        for (double d : l.pattern)
            pat.emplace_back(d);
        rows.push_back(Json(Json::Object{
            {"name", l.name}, {"description", l.description}, {"pattern", Json(std::move(pat))}}));
    }
    return ToolResult::ok(Json(Json::Object{{"linetypes", Json(std::move(rows))}}));
}

ToolResult captureView(ToolContext& ctx, const Json& a) {
    DrawingSession& s = ctx.session;
    const Document& doc = s.document();
    std::string fit = argString(a, "fit", "extents");
    BBox2 area;
    if (fit == "extents") {
        area = drawingExtents(s);
        if (area.empty())
            return ToolResult::fail("the drawing is empty; there is nothing to show");
    } else if (fit == "selection" || fit == "ids") {
        std::vector<EntityId> ids;
        if (fit == "selection") {
            ids = s.selection();
        } else {
            auto parsed = parseIds(doc, a.find("ids"));
            if (!parsed)
                return ToolResult::fail(parsed.error().message);
            ids = std::move(parsed.value());
        }
        for (EntityId id : ids) {
            if (const Entity* e = doc.find(id))
                area.merge(geom::bounds(*e, doc));
        }
        if (area.empty())
            return ToolResult::fail(fit == "selection" ? "nothing is selected"
                                                       : "pass ids of entities that have a shape");
    } else if (fit != "view") {
        return ToolResult::fail("fit must be extents, selection, ids or view");
    }
    if (!area.empty()) {
        double pad = std::max({area.width(), area.height(), 1.0}) * 0.05;
        area = area.inflated(pad);
    }
    int maxPx = static_cast<int>(argNumber(a, "max_size", 1024));
    auto img = s.captureView(maxPx, area);
    if (!img)
        return ToolResult::fail("TBCAD could not render the view right now.");
    ToolResult r = ToolResult::ok(Json(Json::Object{{"width", img->width},
                                                    {"height", img->height},
                                                    {"worldBox", bboxJson(img->worldBox)},
                                                    {"mmPerPixel", img->mmPerPixel}}));
    r.image = std::move(img);
    return r;
}

}  // namespace

void registerReadTools(ToolSet& tools) {
    auto add = [&](Tool t) { tools.add(std::move(t)); };
    auto idArray = [] { return tArray(tString(), "Entity ids (decimal strings)"); };

    add({"get_drawing_info", "Drawing info",
         "Summary of the open drawing: file, units (mm), entity count, extents, current layer, "
         "layer and block counts, undo label, whether the user is in the middle of a command.",
         tObject({}), outSchema({"name", "entityCount", "extents", "currentLayer"}), Edit::None, false,
         true, true, getDrawingInfo});

    add({"list_layers", "List layers",
         "Layer tree. Sub-layers are nested in `children`; `path` joins names with '~' "
         "(S-FLOOR~edge). `effective` shows the state after inheriting from parent layers.",
         tObject({{"include_effective", tBool("default true")}}), outSchema({"layers", "currentLayer"}),
         Edit::None, false, true, true, listLayers});

    add({"query_entities", "Find entities",
         "Entities as JSON, ordered by id. Filter by ids, kinds, layers (paths), or a bounding "
         "box. Paginate with limit and cursor (use next_cursor from the previous page).",
         tObject({{"ids", idArray()},
                  {"kinds", tArray(tEnum({"line", "circle", "arc", "polyline", "point", "ellipse",
                                          "spline", "text", "mtext", "hatch", "insert",
                                          "dimension", "leader", "proxy"}))},
                  {"layers", tArray(tString(), "Layer paths such as S-FLOOR~edge")},
                  {"include_sublayers", tBool("default true")},
                  {"bbox", tArray(tPoint(), "[[x1,y1],[x2,y2]] in mm", 2)},
                  {"mode", tEnum({"inside", "crossing"}, "bbox mode, default crossing")},
                  {"limit", tInt("default 200, max 2000", 1, 2000)},
                  {"cursor", tString("next_cursor of the previous page")}}),
         outSchema({"entities", "total", "next_cursor"}), Edit::None, false, true, true,
         queryEntities});

    add({"get_entity", "Get one entity", "One entity with all its properties.",
         tObject({{"id", tString("entity id")}}, {"id"}), outSchema({"id", "kind", "layer"}),
         Edit::None, false, true, true, getEntity});

    add({"measure", "Measure",
         "distance between two points, area of closed shapes (ids) or of a polygon (points), or "
         "total length of curves (ids). Results in mm and mm^2.",
         tObject({{"kind", tEnum({"distance", "area", "length"})},
                  {"points", tArray(tPoint())},
                  {"ids", idArray()}},
                 {"kind"}),
         outSchema({"distance", "area", "length", "perimeter"}), Edit::None, false, true, true,
         measure});

    add({"get_selection", "Get selection",
         "The entities the user has selected in TBCAD (ids and a count per kind).", tObject({}),
         outSchema({"count", "kinds", "ids"}), Edit::None, false, true, true, getSelection});

    add({"list_commands", "List commands",
         "TBCAD commands with aliases and descriptions; use them with run_command.",
         tObject({{"filter", tString("text to look for in name, alias or description")}}),
         outSchema({"commands"}), Edit::None, false, true, true, listCommands});

    add({"list_blocks", "List blocks", "Block definitions.", tObject({}), outSchema({"blocks"}),
         Edit::None, false, true, true, listBlocks});
    add({"list_text_styles", "List text styles", "Text styles.", tObject({}),
         outSchema({"textStyles"}), Edit::None, false, true, true, listTextStyles});
    add({"list_linetypes", "List linetypes", "Linetypes with dash patterns (mm).", tObject({}),
         outSchema({"linetypes"}), Edit::None, false, true, true, listLinetypes});

    Tool cap{"capture_view", "Capture picture",
             "A PNG picture of the drawing (as the user sees it) so you can check your work. "
             "Does not change the user's view.",
             tObject({{"fit", tEnum({"extents", "selection", "ids", "view"},
                                    "what to show; default extents (the whole drawing)")},
                      {"ids", idArray()},
                      {"max_size", tInt("longer side in pixels, default 1024, max 2048", 64, 2048)}}),
             outSchema({"width", "height", "worldBox", "mmPerPixel"}), Edit::None, false, true, true,
             captureView};
    cap.available = [](DrawingSession& s) { return s.view().has_value(); };
    add(std::move(cap));
}

}  // namespace tbcad::mcp::detail
