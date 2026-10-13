// Structured wrappers around libs/modify: offset, trim, extend, fillet, chamfer, break and join.
// They work on entity ids and points, so an AI does not have to script the interactive commands.

#include <cmath>
#include <set>

#include "tbcad/modify/modify.h"
#include "tools_common.h"

namespace tbcad::mcp::detail {

namespace {

void commit(DrawingSession& s, Transaction& tx, const std::string& tool) {
    ChangeSet cs = tx.commit();
    s.undoStack().push(std::string(kAiLabelPrefix) + tool, {std::move(cs)});
}

struct Pick {
    EntityId id{};
    Vec2 point{};
};

Json changesJson(const ChangeRecorder& rec) {
    return Json(Json::Object{{"created", idsJson(rec.created())},
                             {"modified", idsJson(rec.changed())},
                             {"erased", idsJson(rec.erased())}});
}

// An entity that exists in model space and may be edited.
Result<const Entity*> editable(const Document& doc, const Json& idJson_, const char* what) {
    auto id = parseId(idJson_);
    if (!id)
        return makeError(ErrorCode::InvalidArgument, std::string("bad ") + what + " id");
    if (!doc.contains(*id) || doc.ownerBlock(*id))
        return makeError(ErrorCode::NotFound,
                         std::string("no ") + what + " with id " + std::to_string(id->value));
    const Entity* e = doc.find(*id);
    if (std::string why = whyReadOnly(doc, *e); !why.empty())
        return makeError(ErrorCode::InvalidArgument,
                         std::string(what) + " " + std::to_string(id->value) + ": " + why);
    if (!modify::isCurveEntity(*e))
        return makeError(ErrorCode::InvalidArgument,
                         std::string(what) + " " + std::to_string(id->value) + " is a " +
                             kindName(kind(*e)) +
                             "; only lines, circles, arcs, polylines, "
                             "ellipses and splines can be edited this way");
    return e;
}

Result<Pick> parsePick(const Json& j, const char* what) {
    const Json* id = j.find("id");
    const Json* pt = j.find("pick");
    if (!id || !pt)
        return makeError(ErrorCode::InvalidArgument,
                         std::string(what) + " needs {\"id\": ..., \"pick\": [x, y]}");
    auto i = parseId(*id);
    auto p = pointFromJson(*pt);
    if (!i)
        return makeError(ErrorCode::InvalidArgument, std::string(what) + ": bad id");
    if (!p)
        return p.error();
    return Pick{*i, p.value()};
}

// Edge entities for trim / extend: visible curves (locked layers are fine, nothing changes).
Result<std::vector<const Entity*>> edgeList(const Document& doc, const Json* arr, EntityId self) {
    std::vector<const Entity*> out;
    if (!arr || !arr->isArray() || arr->asArray().empty())
        return makeError(ErrorCode::InvalidArgument, "give at least one edge id");
    for (const Json& j : arr->asArray()) {
        auto id = parseId(j);
        if (!id || !doc.contains(*id) || doc.ownerBlock(*id))
            return makeError(ErrorCode::NotFound, "no edge entity " + dump(j));
        if (*id == self)
            continue;  // an entity cannot cut itself
        const Entity* e = doc.find(*id);
        const Layer* l = doc.layer(e->common.layer);
        if (modify::isCurveEntity(*e) && (!l || (l->on && !l->frozen)))
            out.push_back(e);
    }
    if (out.empty())
        return makeError(ErrorCode::InvalidArgument, "none of the edges is a visible curve");
    return out;
}

Result<void> applyEdit(Transaction& tx, EntityId id, modify::EntityEdit edit) {
    if (edit.eraseSource)
        return tx.erase(id);
    if (edit.modified) {
        const Entity& value = *edit.modified;
        auto r = tx.modify(id, [&](Entity& e) {
            EntityId keep = e.common.id;
            e = value;
            e.common.id = keep;
        });
        if (!r)
            return r;
    }
    for (Entity& e : edit.added) {
        auto r = tx.add(std::move(e));
        if (!r)
            return r.error();
    }
    return {};
}

Result<void> replace(Transaction& tx, EntityId id, const Entity& value) {
    return tx.modify(id, [&](Entity& e) {
        EntityId keep = e.common.id;
        e = value;
        e.common.id = keep;
    });
}

ToolResult offsetEntities(ToolContext& ctx, const Json& a) {
    DrawingSession& s = ctx.session;
    Document& doc = s.document();
    const Json* idsJ = a.find("ids");
    if (!idsJ || !idsJ->isArray() || idsJ->asArray().empty())
        return ToolResult::fail("ids must list at least one entity");
    modify::OffsetOptions opt;
    opt.roundCorners = argBool(a, "round_corners", true);
    const Json* through = a.find("through");
    const Json* sideJ = a.find("side_point");
    std::string side = argString(a, "side");
    double distance = 0;
    if (const Json* d = a.find("distance")) {
        auto l = lengthFromJson(*d);
        if (!l)
            return ToolResult::fail("distance: " + l.error().message);
        distance = l.value();
    }
    if (!through && distance <= 0)
        return ToolResult::fail("distance must be a positive length");
    if (!through && !sideJ && side != "left" && side != "right")
        return ToolResult::fail(
            "say which side: side \"left\" or \"right\" of the direction of "
            "travel, or side_point, or use through");
    std::optional<Vec2> sidePoint, throughPoint;
    if (sideJ) {
        auto p = pointFromJson(*sideJ);
        if (!p)
            return ToolResult::fail("side_point: " + p.error().message);
        sidePoint = p.value();
    }
    if (through) {
        auto p = pointFromJson(*through);
        if (!p)
            return ToolResult::fail("through: " + p.error().message);
        throughPoint = p.value();
    }

    auto tx = doc.begin(std::string(kAiLabelPrefix) + "offset_entities");
    ChangeRecorder rec(doc);
    std::vector<EntityId> sources;
    for (const Json& j : idsJ->asArray()) {
        auto e = editable(doc, j, "entity");
        if (!e)
            return ToolResult::fail(e.error().message);
        Result<std::vector<Entity>> made = std::vector<Entity>{};
        if (throughPoint)
            made = modify::offsetThrough(*e.value(), *throughPoint, opt);
        else if (sidePoint)
            made = modify::offsetToward(*e.value(), *sidePoint, distance, opt);
        else
            made = modify::offsetEntity(*e.value(), side == "left" ? distance : -distance, opt);
        if (!made)
            return ToolResult::fail("entity " + std::to_string(e.value()->common.id.value) + ": " +
                                    made.error().message);
        for (Entity& n : made.value()) {
            auto id = tx.add(std::move(n));
            if (!id)
                return ToolResult::fail(id.error().message);
        }
        sources.push_back(e.value()->common.id);
    }
    if (argBool(a, "delete_source"))
        for (EntityId id : sources)
            if (auto r = tx.erase(id); !r)
                return ToolResult::fail(r.error().message);
    commit(s, tx, "offset_entities");
    return ToolResult::ok(changesJson(rec));
}

ToolResult trimOrExtend(ToolContext& ctx, const Json& a, bool extend) {
    DrawingSession& s = ctx.session;
    Document& doc = s.document();
    const char* tool = extend ? "extend_entities" : "trim_entities";
    const Json* targets = a.find("targets");
    if (!targets || !targets->isArray() || targets->asArray().empty())
        return ToolResult::fail("targets must list at least one {id, pick}");
    modify::TrimOptions opt;
    opt.extendEdges = argBool(a, "extend_edges");
    const Json* edgesJ = a.find(extend ? "boundaries" : "edges");

    auto tx = doc.begin(std::string(kAiLabelPrefix) + tool);
    ChangeRecorder rec(doc);
    std::string failures;
    int done = 0;
    for (const Json& t : targets->asArray()) {
        auto pick = parsePick(t, "target");
        if (!pick)
            return ToolResult::fail(pick.error().message);
        auto target = editable(doc, *t.find("id"), "target");
        if (!target)
            return ToolResult::fail(target.error().message);
        auto edges = edgeList(doc, edgesJ, pick.value().id);
        if (!edges)
            return ToolResult::fail(edges.error().message);
        // The document changes as targets are processed, so look the edges up again each time
        // (edgeList does) and use the target's current state.
        const Entity* current = doc.find(pick.value().id);
        auto edit = extend ? modify::extendEntity(*current, pick.value().point, edges.value(), opt)
                           : modify::trimEntity(*current, pick.value().point, edges.value(), opt);
        if (!edit) {
            failures += (failures.empty() ? "" : "; ") + std::to_string(pick.value().id.value) +
                        ": " + edit.error().message;
            continue;
        }
        if (auto r = applyEdit(tx, pick.value().id, std::move(edit.value())); !r)
            return ToolResult::fail(r.error().message);
        ++done;
    }
    if (done == 0)
        return ToolResult::fail(failures.empty() ? "nothing to do" : failures);
    commit(s, tx, tool);
    Json out = changesJson(rec);
    if (!failures.empty())
        out.object()["skipped"] = failures;
    return ToolResult::ok(std::move(out));
}

ToolResult trimEntities(ToolContext& ctx, const Json& a) {
    return trimOrExtend(ctx, a, false);
}
ToolResult extendEntities(ToolContext& ctx, const Json& a) {
    return trimOrExtend(ctx, a, true);
}

Result<void> applyPair(Transaction& tx, const Pick& a, const Pick& b, modify::PairEdit edit) {
    if (edit.modifiedA)
        if (auto r = replace(tx, a.id, *edit.modifiedA); !r)
            return r;
    if (edit.modifiedB)
        if (auto r = replace(tx, b.id, *edit.modifiedB); !r)
            return r;
    for (Entity& e : edit.added) {
        auto r = tx.add(std::move(e));
        if (!r)
            return r.error();
    }
    return {};
}

ToolResult filletOrChamfer(ToolContext& ctx, const Json& a, bool chamfer) {
    DrawingSession& s = ctx.session;
    Document& doc = s.document();
    const char* tool = chamfer ? "chamfer" : "fillet";
    auto tx = doc.begin(std::string(kAiLabelPrefix) + tool);
    ChangeRecorder rec(doc);

    // A single polyline: round or cut one corner or all of them.
    if (const Json* pl = a.find("polyline")) {
        auto e = editable(doc, *pl, "polyline");
        if (!e)
            return ToolResult::fail(e.error().message);
        if (kind(*e.value()) != EntityKind::Polyline)
            return ToolResult::fail("polyline must be the id of a polyline");
        const Json* vJ = a.find("vertex");
        Result<Entity> out = makeError(ErrorCode::InvalidArgument, "unreachable");
        if (chamfer) {
            modify::ChamferOptions co;
            co.distance1 = argNumber(a, "distance1", 0);
            co.distance2 = argNumber(a, "distance2", co.distance1);
            out = vJ ? modify::chamferPolylineVertex(*e.value(),
                                                     static_cast<std::size_t>(vJ->asNumber()), co)
                     : modify::chamferPolyline(*e.value(), co);
        } else {
            double r = argNumber(a, "radius", 0);
            out = vJ ? modify::filletPolylineVertex(*e.value(),
                                                    static_cast<std::size_t>(vJ->asNumber()), r)
                     : modify::filletPolyline(*e.value(), r);
        }
        if (!out)
            return ToolResult::fail(out.error().message);
        if (auto r = replace(tx, e.value()->common.id, out.value()); !r)
            return ToolResult::fail(r.error().message);
        commit(s, tx, tool);
        return ToolResult::ok(changesJson(rec));
    }

    const Json* f = a.find("first");
    const Json* g = a.find("second");
    if (!f || !g)
        return ToolResult::fail("give `first` and `second` as {id, pick}, or `polyline`");
    auto pa = parsePick(*f, "first");
    auto pb = parsePick(*g, "second");
    if (!pa)
        return ToolResult::fail(pa.error().message);
    if (!pb)
        return ToolResult::fail(pb.error().message);
    auto ea = editable(doc, *f->find("id"), "first");
    auto eb = editable(doc, *g->find("id"), "second");
    if (!ea)
        return ToolResult::fail(ea.error().message);
    if (!eb)
        return ToolResult::fail(eb.error().message);
    Result<modify::PairEdit> edit = makeError(ErrorCode::InvalidArgument, "unreachable");
    if (chamfer) {
        modify::ChamferOptions co;
        co.distance1 = argNumber(a, "distance1", 0);
        co.distance2 = argNumber(a, "distance2", co.distance1);
        if (const Json* ang = a.find("angle")) {
            auto r = angleFromJson(*ang);
            if (!r)
                return ToolResult::fail("angle: " + r.error().message);
            co.byAngle = true;
            co.angle = r.value();
        }
        co.trim = argBool(a, "trim", true);
        edit = modify::chamferEntities(*ea.value(), pa.value().point, *eb.value(), pb.value().point,
                                       co);
    } else {
        modify::FilletOptions fo;
        fo.radius = argNumber(a, "radius", 0);
        fo.trim = argBool(a, "trim", true);
        edit = modify::filletEntities(*ea.value(), pa.value().point, *eb.value(), pb.value().point,
                                      fo);
    }
    if (!edit)
        return ToolResult::fail(edit.error().message);
    if (auto r = applyPair(tx, pa.value(), pb.value(), std::move(edit.value())); !r)
        return ToolResult::fail(r.error().message);
    commit(s, tx, tool);
    return ToolResult::ok(changesJson(rec));
}

ToolResult filletTool(ToolContext& ctx, const Json& a) {
    return filletOrChamfer(ctx, a, false);
}
ToolResult chamferTool(ToolContext& ctx, const Json& a) {
    return filletOrChamfer(ctx, a, true);
}

ToolResult breakEntity(ToolContext& ctx, const Json& a) {
    DrawingSession& s = ctx.session;
    Document& doc = s.document();
    const Json* idJ = a.find("id");
    const Json* p1J = a.find("point1");
    if (!idJ || !p1J)
        return ToolResult::fail("give id and point1 (and point2 to remove the part between)");
    auto e = editable(doc, *idJ, "entity");
    if (!e)
        return ToolResult::fail(e.error().message);
    auto p1 = pointFromJson(*p1J);
    if (!p1)
        return ToolResult::fail("point1: " + p1.error().message);
    Result<modify::EntityEdit> edit = makeError(ErrorCode::InvalidArgument, "unreachable");
    if (const Json* p2J = a.find("point2")) {
        auto p2 = pointFromJson(*p2J);
        if (!p2)
            return ToolResult::fail("point2: " + p2.error().message);
        edit = modify::breakEntity(*e.value(), p1.value(), p2.value());
    } else {
        edit = modify::breakAtPoint(*e.value(), p1.value());
    }
    if (!edit)
        return ToolResult::fail(edit.error().message);
    auto tx = doc.begin(std::string(kAiLabelPrefix) + "break_entity");
    ChangeRecorder rec(doc);
    if (auto r = applyEdit(tx, e.value()->common.id, std::move(edit.value())); !r)
        return ToolResult::fail(r.error().message);
    commit(s, tx, "break_entity");
    return ToolResult::ok(changesJson(rec));
}

ToolResult joinEntities(ToolContext& ctx, const Json& a) {
    DrawingSession& s = ctx.session;
    Document& doc = s.document();
    const Json* idsJ = a.find("ids");
    if (!idsJ || !idsJ->isArray() || idsJ->asArray().size() < 2)
        return ToolResult::fail("ids must list at least two entities");
    std::vector<const Entity*> list;
    std::set<EntityId> seen;
    for (const Json& j : idsJ->asArray()) {
        auto e = editable(doc, j, "entity");
        if (!e)
            return ToolResult::fail(e.error().message);
        if (seen.insert(e.value()->common.id).second)
            list.push_back(e.value());
    }
    double tol = 1e-4;
    if (const Json* t = a.find("tolerance")) {
        auto l = lengthFromJson(*t);
        if (!l)
            return ToolResult::fail("tolerance: " + l.error().message);
        tol = l.value();
    }
    modify::JoinResult jr = modify::joinEntities(list, tol);
    if (jr.consumed.empty())
        return ToolResult::fail("nothing to join: the entities do not touch end to end");
    auto tx = doc.begin(std::string(kAiLabelPrefix) + "join_entities");
    ChangeRecorder rec(doc);
    for (Entity& e : jr.created) {
        auto id = tx.add(std::move(e));
        if (!id)
            return ToolResult::fail(id.error().message);
    }
    for (std::size_t i : jr.consumed)
        if (auto r = tx.erase(list[i]->common.id); !r)
            return ToolResult::fail(r.error().message);
    commit(s, tx, "join_entities");
    return ToolResult::ok(changesJson(rec));
}

}  // namespace

void registerModifyTools(ToolSet& tools) {
    auto idArray = [](const char* d = "Entity ids (decimal strings)") {
        return tArray(tString(), d);
    };
    Json pickSchema =
        tObject({{"id", tString("entity id")}, {"pick", tPoint("point near the part to act on")}},
                {"id", "pick"});
    Json changes = outSchema({"created", "modified", "erased"});

    tools.add({"offset_entities", "Offset entities",
               "Makes parallel copies of lines, circles, arcs, polylines, ellipses and splines at "
               "`distance`, to the `side` (left or right of the direction of travel) or toward "
               "`side_point`; or through `through` (distance taken from that point). New "
               "entities keep the source's properties. One undo step.",
               tObject({{"ids", idArray()},
                        {"distance", tAny("positive length")},
                        {"side", tEnum({"left", "right"})},
                        {"side_point", tPoint("offset toward the side this point is on")},
                        {"through", tPoint("offset to the curve through this point")},
                        {"round_corners", tBool("polylines: arcs at convex corners (default)")},
                        {"delete_source", tBool()}},
                       {"ids"}),
               changes, Edit::Document, false, false, true, offsetEntities});

    tools.add({"trim_entities", "Trim entities",
               "Trims each target back to the nearest cutting `edges` on both sides of its `pick` "
               "point. Lines, arcs, circles, ellipses and polylines. Targets that cannot be "
               "trimmed are reported in `skipped`; the rest are done in one undo step.",
               tObject({{"targets", tArray(pickSchema, "", 1000)},
                        {"edges", idArray("cutting edge ids")},
                        {"extend_edges", tBool("treat edges as if they ran on")}},
                       {"targets", "edges"}),
               changes, Edit::Document, false, false, true, trimEntities});

    tools.add({"extend_entities", "Extend entities",
               "Extends the end of each target nearest its `pick` point to the nearest boundary "
               "ahead of it. Lines, arcs and open polylines.",
               tObject({{"targets", tArray(pickSchema, "", 1000)},
                        {"boundaries", idArray("boundary edge ids")},
                        {"extend_edges", tBool("treat boundaries as if they ran on")}},
                       {"targets", "boundaries"}),
               changes, Edit::Document, false, false, true, extendEntities});

    tools.add({"fillet", "Fillet",
               "Rounds the corner of two lines, arcs or circles with `radius` (0 = sharp corner), "
               "or of one polyline (`polyline`, optional `vertex`; all corners without it).",
               tObject({{"first", pickSchema},
                        {"second", pickSchema},
                        {"polyline", tString("polyline id instead of first/second")},
                        {"vertex", tInt("polyline corner index")},
                        {"radius", tNumber()},
                        {"trim", tBool("trim or extend the inputs (default true)")}}),
               changes, Edit::Document, false, false, true, filletTool});

    tools.add({"chamfer", "Chamfer",
               "Cuts the corner of two straight lines (`distance1` along the first, `distance2` "
               "along the second, or `distance1` and `angle`), or of a polyline.",
               tObject({{"first", pickSchema},
                        {"second", pickSchema},
                        {"polyline", tString("polyline id instead of first/second")},
                        {"vertex", tInt("polyline corner index")},
                        {"distance1", tNumber()},
                        {"distance2", tNumber()},
                        {"angle", tAny("with distance1: angle to the first line")},
                        {"trim", tBool()}}),
               changes, Edit::Document, false, false, true, chamferTool});

    tools.add(
        {"break_entity", "Break entity",
         "With point1 and point2: removes the part between them. With point1 only: splits "
         "the entity in two there.",
         tObject({{"id", tString()}, {"point1", tPoint()}, {"point2", tPoint()}}, {"id", "point1"}),
         changes, Edit::Document, false, false, true, breakEntity});

    tools.add({"join_entities", "Join entities",
               "Joins lines, arcs and polylines that touch end to end (and overlapping collinear "
               "lines) into one line, arc, circle or polyline.",
               tObject({{"ids", idArray()}, {"tolerance", tAny("end point gap, default 0.0001")}},
                       {"ids"}),
               changes, Edit::Document, true, false, true, joinEntities});
}

}  // namespace tbcad::mcp::detail
