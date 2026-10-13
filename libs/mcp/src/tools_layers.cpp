#include <functional>

#include "tools_common.h"

namespace tbcad::mcp::detail {

namespace {

const std::string kLabelPrefix = kAiLabelPrefix;

void commit(DrawingSession& s, Transaction& tx, const std::string& tool) {
    ChangeSet cs = tx.commit();
    s.undoStack().push(kLabelPrefix + tool, {std::move(cs)});
}

// Applies the optional layer property arguments. Returns an error text or empty.
std::string applyLayerProps(const Document& doc, Layer& row, const Json& a) {
    if (const Json* v = a.find("color")) {
        auto r = colorFromJson(*v, true);
        if (!r)
            return r.error().message;
        row.color = r.value();
    }
    if (const Json* v = a.find("linetype")) {
        auto r = linetypeFromJson(doc, *v, true);
        if (!r)
            return r.error().message;
        row.linetype = r.value();
    }
    if (const Json* v = a.find("lineweight")) {
        auto r = lineweightFromJson(*v, true);
        if (!r)
            return r.error().message;
        row.lineweight = r.value();
    }
    if (const Json* v = a.find("on"))
        row.on = v->asBool(true);
    if (const Json* v = a.find("frozen"))
        row.frozen = v->asBool(false);
    if (const Json* v = a.find("locked"))
        row.locked = v->asBool(false);
    if (const Json* v = a.find("plot"))
        row.plot = v->asBool(true);
    return {};
}

std::size_t entitiesOnLayer(const Document& doc, LayerId id) {
    std::size_t n = 0;
    doc.forEachEntity([&](const Entity& e) {
        if (e.common.layer == id)
            ++n;
    });
    return n;
}

Json layerInfo(const Document& doc, LayerId id) {
    const Layer& l = *doc.layer(id);
    return Json(Json::Object{{"path", layerPath(doc, id)},
                             {"color", colorToJson(l.color)},
                             {"linetype", linetypeToJson(doc, l.linetype)},
                             {"lineweight", lineweightToJson(l.lineweight)},
                             {"on", l.on},
                             {"frozen", l.frozen},
                             {"locked", l.locked},
                             {"plot", l.plot}});
}

ToolResult createLayer(ToolContext& ctx, const Json& a) {
    DrawingSession& s = ctx.session;
    Document& doc = s.document();
    std::string name = argString(a, "name");
    if (name.find('~') != std::string::npos)
        return ToolResult::fail(
            "a layer name cannot contain '~' (it separates sub-layer names in "
            "paths); use the `parent` argument to make a sub-layer");
    Layer row;
    row.name = name;
    std::string parentPath = argString(a, "parent");
    if (!parentPath.empty()) {
        const Layer* p = findLayerByPath(doc, parentPath);
        if (!p)
            return ToolResult::fail("unknown parent layer \"" + parentPath + "\"");
        row.parent = p->id;
        row.color = Color::byParent();
        row.linetype = kLinetypeByParent;
        row.lineweight = LineWeight::byParent();
    }
    if (doc.layerByName(row.parent, name))
        return ToolResult::fail("layer \"" + name + "\" already exists" +
                                (parentPath.empty() ? "" : " under " + parentPath));
    std::string err = applyLayerProps(doc, row, a);
    if (!err.empty())
        return ToolResult::fail(err);
    auto tx = doc.begin(kLabelPrefix + "create_layer");
    auto id = tx.addLayer(row);
    if (!id)
        return ToolResult::fail(id.error().message);
    if (argBool(a, "make_current")) {
        LayerId cur = id.value();
        auto m = tx.modifyHeader([&](Header& h) { h.currentLayer = cur; });
        if (!m)
            return ToolResult::fail(m.error().message);
    }
    commit(s, tx, "create_layer");
    return ToolResult::ok(layerInfo(doc, id.value()));
}

// Gives a sub-layer explicit values for everything it takes from its parent.
void resolveByParent(const Document& doc, Layer& row) {
    EffectiveLayer eff = doc.effective(row.id);
    if (row.color.kind == ColorKind::ByParent)
        row.color = eff.color;
    if (row.linetype == kLinetypeByParent)
        row.linetype = eff.linetype;
    if (row.lineweight.isByParent())
        row.lineweight = eff.lineweight;
}

ToolResult modifyLayer(ToolContext& ctx, const Json& a) {
    DrawingSession& s = ctx.session;
    Document& doc = s.document();
    std::string path = argString(a, "path");
    const Layer* layer = findLayerByPath(doc, path);
    if (!layer)
        return ToolResult::fail("unknown layer \"" + path + "\"");
    LayerId id = layer->id;
    Layer probe = *layer;
    std::string err = applyLayerProps(doc, probe, a);
    if (!err.empty())
        return ToolResult::fail(err);

    std::optional<std::string> rename;
    if (const Json* r = a.find("rename")) {
        if (!r->isString() || r->asString().find('~') != std::string::npos)
            return ToolResult::fail("rename must be a name without '~'");
        rename = r->asString();
    }
    std::optional<LayerId> newParent;
    if (const Json* p = a.find("new_parent")) {
        if (p->isNull() || (p->isString() && p->asString().empty())) {
            newParent = LayerId{};
        } else {
            const Layer* np = p->isString() ? findLayerByPath(doc, p->asString()) : nullptr;
            if (!np)
                return ToolResult::fail("unknown new_parent " + dump(*p));
            newParent = np->id;
        }
    }
    auto tx = doc.begin(kLabelPrefix + "modify_layer");
    std::string failure;
    auto m = tx.modifyLayer(id, [&](Layer& l) {
        // Property arguments, applied again on the live row.
        (void)applyLayerProps(doc, l, a);
        if (rename)
            l.name = *rename;
        if (newParent) {
            if (!*newParent)
                resolveByParent(doc, l);  // a top-level layer cannot use ByParent
            l.parent = *newParent;
        }
    });
    if (!m)
        return ToolResult::fail(m.error().message);
    commit(s, tx, "modify_layer");
    return ToolResult::ok(layerInfo(doc, id));
}

ToolResult deleteLayer(ToolContext& ctx, const Json& a) {
    DrawingSession& s = ctx.session;
    Document& doc = s.document();
    std::string path = argString(a, "path");
    const Layer* layer = findLayerByPath(doc, path);
    if (!layer)
        return ToolResult::fail("unknown layer \"" + path + "\"");
    if (layer->id == kLayerZero)
        return ToolResult::fail("layer \"0\" cannot be deleted");
    bool moveUp = argString(a, "mode", "branch") == "move_children_up";

    // The layer and its descendants, deepest first.
    std::vector<LayerId> order;
    std::function<void(LayerId)> walk = [&](LayerId id) {
        for (const auto& [cid, c] : doc.layers()) {
            if (c.parent == id)
                walk(cid);
        }
        order.push_back(id);
    };
    walk(layer->id);

    auto blocker = [&](LayerId id) -> std::string {
        if (std::size_t n = entitiesOnLayer(doc, id))
            return "layer \"" + layerPath(doc, id) + "\" still holds " + std::to_string(n) +
                   " entities; move or delete them first";
        return {};
    };

    for (LayerId id : moveUp ? std::vector<LayerId>{layer->id} : order) {
        if (doc.header().currentLayer == id)
            return ToolResult::fail("layer \"" + layerPath(doc, id) +
                                    "\" is the current layer; make another layer current first");
    }
    auto tx = doc.begin(kLabelPrefix + "delete_layer");
    std::size_t removed = 0;
    if (moveUp) {
        if (std::string b = blocker(layer->id); !b.empty())
            return ToolResult::fail(b);
        LayerId target = layer->parent;
        std::vector<LayerId> kids;
        for (const auto& [cid, c] : doc.layers()) {
            if (c.parent == layer->id)
                kids.push_back(cid);
        }
        for (LayerId k : kids) {
            auto m = tx.modifyLayer(k, [&](Layer& l) {
                if (!target)
                    resolveByParent(doc, l);
                l.parent = target;
            });
            if (!m)
                return ToolResult::fail(m.error().message);
        }
        auto e = tx.eraseLayer(layer->id);
        if (!e)
            return ToolResult::fail(e.error().message);
        removed = 1;
    } else {
        for (LayerId id : order) {
            if (std::string b = blocker(id); !b.empty())
                return ToolResult::fail(b);
        }
        for (LayerId id : order) {
            auto e = tx.eraseLayer(id);
            if (!e)
                return ToolResult::fail(e.error().message);
            ++removed;
        }
    }
    commit(s, tx, "delete_layer");
    return ToolResult::ok(Json(Json::Object{{"deleted", static_cast<double>(removed)}}));
}

ToolResult setCurrentLayer(ToolContext& ctx, const Json& a) {
    DrawingSession& s = ctx.session;
    Document& doc = s.document();
    std::string path = argString(a, "path");
    const Layer* layer = findLayerByPath(doc, path);
    if (!layer)
        return ToolResult::fail("unknown layer \"" + path + "\"");
    LayerId id = layer->id;
    auto tx = doc.begin(kLabelPrefix + "set_current_layer");
    auto m = tx.modifyHeader([&](Header& h) { h.currentLayer = id; });
    if (!m)
        return ToolResult::fail(m.error().message);
    commit(s, tx, "set_current_layer");
    return ToolResult::ok(Json(Json::Object{{"currentLayer", layerPath(doc, id)}}));
}

ToolResult setActiveOnly(ToolContext& ctx, const Json& a) {
    DrawingSession& s = ctx.session;
    Document& doc = s.document();
    bool on = argBool(a, "on");
    auto tx = doc.begin(kLabelPrefix + "set_active_only");
    auto m = tx.modifyHeader([&](Header& h) { h.activeOnly = on; });
    if (!m)
        return ToolResult::fail(m.error().message);
    commit(s, tx, "set_active_only");
    return ToolResult::ok(Json(Json::Object{{"activeOnly", on}}));
}

}  // namespace

void registerLayerTools(ToolSet& tools) {
    Json::Object props{
        {"color", tAny("ACI number 1..255 or \"#RRGGBB\"; sub-layers also \"ByParent\"")},
        {"linetype", tString("linetype name; sub-layers also \"ByParent\"")},
        {"lineweight", tAny("mm, \"Default\"; sub-layers also \"ByParent\"")},
        {"on", tBool()},
        {"frozen", tBool()},
        {"locked", tBool()},
        {"plot", tBool()}};

    Json::Object createProps = props;
    createProps["name"] = tString("layer name (no '~')");
    createProps["parent"] = tString("path of the parent layer; makes this a sub-layer");
    createProps["make_current"] = tBool();
    tools.add({"create_layer", "Create layer",
               "Creates a layer, or a sub-layer when `parent` is given (up to 8 levels). A new "
               "sub-layer inherits colour, linetype and lineweight from its parent (ByParent) "
               "unless you set them.",
               tObject(createProps, {"name"}), outSchema({"path"}), Edit::Document, false, false,
               true, createLayer});

    Json::Object modifyProps = props;
    modifyProps["path"] = tString("layer path, e.g. S-FLOOR~edge");
    modifyProps["rename"] = tString("new name (children keep theirs; their paths change)");
    modifyProps["new_parent"] = tString("path of the new parent, or \"\" for top level");
    tools.add({"modify_layer", "Modify layer",
               "Changes a layer's colour, linetype, lineweight, on/frozen/locked/plot, name or "
               "parent. Refuses cycles and nesting deeper than 8.",
               tObject(modifyProps, {"path"}), outSchema({"path"}), Edit::Document, false, false,
               true, modifyLayer});

    tools.add(
        {"delete_layer", "Delete layer",
         "Deletes an empty layer. mode `branch` (default) also deletes its sub-layers (all "
         "must be empty); `move_children_up` keeps the sub-layers one level higher.",
         tObject({{"path", tString()}, {"mode", tEnum({"branch", "move_children_up"})}}, {"path"}),
         outSchema({"deleted"}), Edit::Document, true, false, true, deleteLayer});

    tools.add({"set_current_layer", "Set current layer",
               "New entities drawn by the user and by commands go on this layer.",
               tObject({{"path", tString()}}, {"path"}), outSchema({"currentLayer"}),
               Edit::Document, false, true, true, setCurrentLayer});

    tools.add({"set_active_only", "Set Active Only mode",
               "When on, only the current layer and its sub-layers can be edited: entities on "
               "other layers cannot be modified or deleted and nothing can be added to them.",
               tObject({{"on", tBool()}}, {"on"}), outSchema({"activeOnly"}), Edit::Document, false,
               true, true, setActiveOnly});
}

}  // namespace tbcad::mcp::detail
