#include <algorithm>
#include <array>
#include <cmath>

#include "commands.h"
#include "tbcad/blocks/tbcad_blocks.h"
#include "util.h"

namespace tbcad::modifycmd {

namespace {

bool selectable(const Document& doc, const Entity& e) {
    return isEditable(doc, e);
}

cmd::EntityFilterFn editableFilter(const Document& doc) {
    return [&doc](const Entity& e) { return selectable(doc, e); };
}

cmd::EntityFilterFn modifiableFilter(const Document& doc) {
    return [&doc](const Entity& e) { return isModifiable(doc, e); };
}

std::string plural(std::size_t n, const char* one, const char* many) {
    return std::to_string(n) + " " + (n == 1 ? one : many);
}

// ---- LENGTHEN -----------------------------------------------------------------------------------

class LengthenCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "LENGTHEN"; }
    std::vector<std::string> defaultAliases() const override { return {"LEN"}; }
    std::string_view description() const override {
        return "Change the length of lines, arcs and open polylines";
    }

    cmd::Task run(cmd::CommandContext& ctx) override {
        using modify::LengthenMode;
        ctx.commitOnCancel();
        ctx.setUndoLabel("LENGTHEN");
        std::string mode = "Delta";
        for (;;) {
            cmd::EntityOptions eo;
            eo.keywords = {"Delta", "Percent", "Total", "Dynamic"};
            eo.filter = [&doc = ctx.doc()](const Entity& e) {
                return selectable(doc, e) && modify::lengthenableLength(e).has_value();
            };
            auto pick = co_await ctx.getEntity(
                "Select an object to lengthen or [Delta/Percent/Total/Dynamic]", eo);
            if (pick.status == cmd::Status::Keyword) {
                mode = pick.keyword;
                if (mode == "Delta")
                    mode = "Delta";
                if (mode == "Dynamic")
                    mode = "Dynamic";
                if (mode == "Delta") {
                    cmd::DistanceOptions d;
                    d.allowNegative = true;
                    d.defaultValue = getSetting(ctx.host(), "lengthen.delta", 10);
                    auto r = co_await ctx.getDistance("Enter delta length", d);
                    if (!r.ok())
                        co_return;
                    putSetting(ctx.host(), "lengthen.delta", r.value);
                    delta_ = r.value;
                } else if (mode == "Percent") {
                    cmd::RealOptions ro;
                    ro.defaultValue = getSetting(ctx.host(), "lengthen.percent", 100);
                    ro.min = 0.001;
                    auto r = co_await ctx.getReal("Enter percentage length", ro);
                    if (!r.ok())
                        co_return;
                    putSetting(ctx.host(), "lengthen.percent", r.value);
                    percent_ = r.value;
                } else if (mode == "Total") {
                    cmd::DistanceOptions d;
                    d.allowZero = false;
                    auto r = co_await ctx.getDistance("Specify total length", d);
                    if (!r.ok())
                        co_return;
                    total_ = r.value;
                }
                continue;
            }
            if (!pick.ok())
                co_return;
            const Entity* e = ctx.doc().find(pick.id);
            if (!e)
                continue;
            Result<modify::EntityEdit> edit = Error{ErrorCode::InvalidArgument, "Unsupported"};
            if (mode == "Delta") {
                edit = modify::lengthenEntity(*e, pick.pickPoint, LengthenMode::Delta, delta_);
            } else if (mode == "Percent") {
                edit = modify::lengthenEntity(*e, pick.pickPoint, LengthenMode::Percent, percent_);
            } else if (mode == "Total") {
                edit = modify::lengthenEntity(*e, pick.pickPoint, LengthenMode::Total, total_);
            } else {
                cmd::PointOptions po;
                auto p = co_await ctx.getPoint("Specify new end point", po);
                if (!p.ok())
                    continue;
                edit = modify::lengthenToPoint(*e, pick.pickPoint, p.point);
            }
            if (!edit) {
                ctx.message(edit.error().message);
                continue;
            }
            if (auto ids = applyEdit(ctx.tx(), pick.id, std::move(edit.value())); !ids)
                ctx.message(ids.error().message);
        }
    }

private:
    double delta_ = 10, percent_ = 100, total_ = 0;
};

// ---- REVERSE ------------------------------------------------------------------------------------

class ReverseCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "REVERSE"; }
    std::string_view description() const override {
        return "Reverse the direction of lines, polylines and splines";
    }

    cmd::Task run(cmd::CommandContext& ctx) override {
        cmd::SelectionOptions so;
        so.filter = editableFilter(ctx.doc());
        auto sel = co_await ctx.getSelection("Select objects to reverse", so);
        if (!sel.ok())
            co_return;
        std::size_t done = 0;
        for (EntityId id : sel.ids) {
            const Entity* e = ctx.doc().find(id);
            if (!e)
                continue;
            auto edit = modify::reverseEntity(*e);
            if (!edit)
                continue;
            if (applyEdit(ctx.tx(), id, std::move(edit.value())))
                ++done;
        }
        ctx.message(done ? plural(done, "object", "objects") + " reversed."
                         : "None of the selected objects has a direction.");
    }
};

// ---- DIVIDE / MEASURE ---------------------------------------------------------------------------

// Adds the points, selects them (a default point is a single pixel, so they would otherwise be
// hard to see) and, while the point style is still the default dot, says how to make them bigger.
Result<void> addPoints(cmd::CommandContext& ctx, const std::vector<Vec2>& pts, const Entity& like) {
    std::vector<EntityId> made;
    struct Finish {
        cmd::CommandContext& ctx;
        std::vector<EntityId>& made;
        ~Finish() {
            if (made.empty())
                return;
            ctx.host().setSelection(made);
            if (ctx.doc().header().custom.find("pdmode") == ctx.doc().header().custom.end())
                ctx.message(
                    "The points are drawn as tiny dots. Use PTYPE to choose a bigger "
                    "point style.");
        }
    } finish{ctx, made};
    for (Vec2 p : pts) {
        Entity pt;
        pt.common = like.common;
        pt.common.id = {};
        pt.data = PointData{p};
        auto r = ctx.tx().add(std::move(pt));
        if (!r)
            return r.error();
        made.push_back(r.value());
    }
    return Result<void>{};
}

// DIVIDE and MEASURE with the Block option: inserts of a block instead of points, turned along the
// object when `align` is set.
Result<void> addBlocks(cmd::CommandContext& ctx, const std::vector<Vec2>& pts, const Entity& like,
                       BlockId block, bool align) {
    std::vector<geom::CurveRef> curves;
    const bool haveCurves = geom::curvesOf(like, curves) && !curves.empty();
    std::vector<EntityId> made;
    for (Vec2 p : pts) {
        double rotation = 0;
        if (align && haveCurves) {
            const geom::CurveRef* best = &curves.front();
            double bestDist = best->distanceTo(p);
            for (const geom::CurveRef& c : curves)
                if (const double d = c.distanceTo(p); d < bestDist) {
                    best = &c;
                    bestDist = d;
                }
            const Vec2 t = best->tangentAt(best->paramAt(p));
            if (t.x != 0 || t.y != 0)
                rotation = std::atan2(t.y, t.x);
        }
        InsertData data{block, p, {1, 1}, rotation};
        data.attribs = blocks::defaultAttribs(ctx.doc(), block);
        Entity ins;
        ins.common = like.common;
        ins.common.id = {};
        ins.common.color = Color::byLayer();
        ins.common.linetype = kLinetypeByLayer;
        ins.common.lineweight = LineWeight::byLayer();
        ins.data = Indirect<InsertData>(std::move(data));
        auto r = ctx.tx().add(std::move(ins));
        if (!r)
            return r.error();
        made.push_back(r.value());
    }
    ctx.host().setSelection(made);
    return Result<void>{};
}

// Asks which block to place and whether to turn it along the object. `ok` is false on cancel.
cmd::Task askBlockOption(cmd::CommandContext& ctx, BlockId& block, bool& align, bool& ok) {
    ok = false;
    for (;;) {
        auto name = co_await ctx.getString("Enter name of block to insert");
        if (!name.ok())
            co_return;
        if (const BlockDef* def = ctx.doc().blockByName(cmd::trim(name.text))) {
            block = def->id;
            break;
        }
        ctx.message("No block named \"" + cmd::trim(name.text) + "\".");
    }
    auto yn = co_await ctx.getKeyword(
        "Align block with object? [Yes/No] <Yes>",
        cmd::KeywordOptions{.keywords = {"Yes", "No"}, .defaultKeyword = "Yes"});
    if (!yn.ok())
        co_return;
    align = yn.keyword == "Yes";
    ok = true;
}

bool dividable(const Entity& e) {
    switch (kind(e)) {
        case EntityKind::Line:
        case EntityKind::Circle:
        case EntityKind::Arc:
        case EntityKind::Polyline:
        case EntityKind::Ellipse:
        case EntityKind::Spline:
            return true;
        default:
            return false;
    }
}

class DivideCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "DIVIDE"; }
    std::vector<std::string> defaultAliases() const override { return {"DIV"}; }
    std::string_view description() const override {
        return "Place points that divide an object into equal parts";
    }

    cmd::Task run(cmd::CommandContext& ctx) override {
        cmd::EntityOptions eo;
        eo.filter = [&doc = ctx.doc()](const Entity& e) {
            return doc.layer(e.common.layer) && dividable(e);
        };
        auto pick = co_await ctx.getEntity("Select object to divide", eo);
        if (!pick.ok())
            co_return;
        cmd::IntOptions io;
        io.min = 2;
        io.max = 32767;
        BlockId block{};
        bool align = true;
        io.keywords = {"Block"};
        auto n = co_await ctx.getInteger("Enter the number of segments or [Block]", io);
        if (n.status == cmd::Status::Keyword) {
            bool ok = false;
            co_await askBlockOption(ctx, block, align, ok);
            if (!ok)
                co_return;
            io.keywords = {};
            n = co_await ctx.getInteger("Enter the number of segments", io);
        }
        if (!n.ok())
            co_return;
        const Entity* e = ctx.doc().find(pick.id);
        if (!e)
            co_return;
        auto pts = modify::dividePoints(*e, static_cast<int>(n.value));
        if (!pts) {
            ctx.message(pts.error().message);
            co_return;
        }
        Entity like = *e;
        like.common.color = Color::byLayer();
        like.common.linetype = kLinetypeByLayer;
        like.common.lineweight = LineWeight::byLayer();
        const bool blocksOut = static_cast<bool>(block);
        if (auto r = blocksOut ? addBlocks(ctx, pts.value(), *e, block, align)
                               : addPoints(ctx, pts.value(), like);
            !r)
            ctx.message(r.error().message);
        else
            ctx.message(plural(pts.value().size(), blocksOut ? "block" : "point",
                               blocksOut ? "blocks" : "points") +
                        " placed.");
    }
};

class MeasureCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "MEASURE"; }
    std::vector<std::string> defaultAliases() const override { return {"ME"}; }
    std::string_view description() const override {
        return "Place points at a fixed distance along an object";
    }

    cmd::Task run(cmd::CommandContext& ctx) override {
        cmd::EntityOptions eo;
        eo.filter = [&doc = ctx.doc()](const Entity& e) {
            return doc.layer(e.common.layer) && dividable(e);
        };
        auto pick = co_await ctx.getEntity("Select object to measure", eo);
        if (!pick.ok())
            co_return;
        cmd::DistanceOptions d;
        d.allowZero = false;
        d.keywords = {"Block"};
        BlockId block{};
        bool align = true;
        auto len = co_await ctx.getDistance("Specify length of segment or [Block]", d);
        if (len.status == cmd::Status::Keyword) {
            bool ok = false;
            co_await askBlockOption(ctx, block, align, ok);
            if (!ok)
                co_return;
            d.keywords = {};
            len = co_await ctx.getDistance("Specify length of segment", d);
        }
        if (!len.ok())
            co_return;
        const Entity* e = ctx.doc().find(pick.id);
        if (!e)
            co_return;
        // Measuring starts at the end nearest the pick.
        bool fromEnd = false;
        std::vector<geom::CurveRef> cs;
        if (geom::curvesOf(*e, cs) && !cs.empty()) {
            Vec2 first = cs.front().pointAt(cs.front().t0());
            Vec2 last = cs.back().pointAt(cs.back().t0() + cs.back().sweep());
            fromEnd = distance(pick.pickPoint, last) < distance(pick.pickPoint, first);
        }
        auto pts = modify::measurePoints(*e, len.value, fromEnd);
        if (!pts) {
            ctx.message(pts.error().message);
            co_return;
        }
        Entity like = *e;
        like.common.color = Color::byLayer();
        like.common.linetype = kLinetypeByLayer;
        like.common.lineweight = LineWeight::byLayer();
        const bool blocksOut = static_cast<bool>(block);
        if (auto r = blocksOut ? addBlocks(ctx, pts.value(), *e, block, align)
                               : addPoints(ctx, pts.value(), like);
            !r)
            ctx.message(r.error().message);
        else
            ctx.message(plural(pts.value().size(), blocksOut ? "block" : "point",
                               blocksOut ? "blocks" : "points") +
                        " placed.");
    }
};

// ---- ALIGN --------------------------------------------------------------------------------------

class AlignCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "ALIGN"; }
    std::vector<std::string> defaultAliases() const override { return {"AL"}; }
    std::string_view description() const override {
        return "Move and rotate objects to line up with other objects";
    }

    cmd::Task run(cmd::CommandContext& ctx) override {
        cmd::SelectionOptions so;
        so.filter = modifiableFilter(ctx.doc());
        auto sel = co_await ctx.getSelection("Select objects to align", so);
        if (!sel.ok())
            co_return;
        auto s1 = co_await ctx.getPoint("Specify first source point");
        if (!s1.ok())
            co_return;
        cmd::PointOptions po1;
        po1.base = s1.point;
        auto d1 = co_await ctx.getPoint("Specify first destination point", po1);
        if (!d1.ok())
            co_return;
        cmd::PointOptions po2;
        po2.allowNone = true;
        auto s2 = co_await ctx.getPoint("Specify second source point or <continue>", po2);
        std::optional<Vec2> src2, dst2;
        bool scale = false;
        if (s2.status == cmd::Status::Ok) {
            cmd::PointOptions po3;
            po3.base = s2.point;
            auto d2 = co_await ctx.getPoint("Specify second destination point", po3);
            if (!d2.ok())
                co_return;
            src2 = s2.point;
            dst2 = d2.point;
            cmd::KeywordOptions ko;
            ko.keywords = {"Yes", "No"};
            ko.defaultKeyword = "No";
            auto k = co_await ctx.getKeyword(
                "Scale objects based on alignment points? [Yes/No] <No>", ko);
            if (k.status == cmd::Status::Cancel)
                co_return;
            scale = k.keyword == "Yes";
        } else if (s2.status == cmd::Status::Cancel) {
            co_return;
        }
        auto t = modify::alignTransform(s1.point, d1.point, src2, dst2, scale);
        if (!t) {
            ctx.message(t.error().message);
            co_return;
        }
        std::size_t done = 0;
        for (EntityId id : sel.ids) {
            const Entity* e = ctx.doc().find(id);
            if (!e)
                continue;
            auto moved = geom::transformed(*e, t.value());
            if (!moved) {
                ctx.message(moved.error().message);
                continue;
            }
            Entity v = std::move(moved).value();
            if (ctx.tx().modify(id, [&](Entity& x) { x.data = std::move(v.data); }))
                ++done;
        }
        ctx.message(plural(done, "object", "objects") + " aligned.");
    }
};

// ---- MATCHPROP ----------------------------------------------------------------------------------

struct MatchCats {
    bool basic = true;       // layer, colour, linetype, scale, weight, transparency, level
    bool text = true;        // text and MText height, style, width, slant
    bool dimension = true;   // dimension, leader and multileader style and text height
    bool hatch = true;       // hatch pattern, scale, angle, fill
    bool polyline = true;    // polyline width
    bool annotative = true;  // annotative flag and scales
};

constexpr std::array<const char*, 6> kMatchCats = {"Basic", "Text",     "Dimension",
                                                   "Hatch", "Polyline", "Annotative"};

bool& matchFlag(MatchCats& c, std::size_t i) {
    bool* all[] = {&c.basic, &c.text, &c.dimension, &c.hatch, &c.polyline, &c.annotative};
    return *all[i];
}

MatchCats readMatchCats(const cmd::IHost& host) {
    MatchCats c;
    for (std::size_t i = 0; i < kMatchCats.size(); ++i)
        matchFlag(c, i) = getFlag(host, std::string("matchprop.") + kMatchCats[i], true);
    return c;
}

void matchProps(const Entity& src, Entity& dst, const MatchCats& cats = {}) {
    if (cats.basic) {
        dst.common.layer = src.common.layer;
        dst.common.level = src.common.level;
        dst.common.color = src.common.color;
        dst.common.linetype = src.common.linetype;
        dst.common.linetypeScale = src.common.linetypeScale;
        dst.common.lineweight = src.common.lineweight;
        dst.common.transparency = src.common.transparency;
    }
    if (cats.annotative) {
        dst.common.annotative = src.common.annotative;
        dst.common.annoScales = src.common.annoScales;
    }
    if (cats.text) {
        if (const auto* a = entityAs<TextData>(src)) {
            if (auto* b = entityAs<TextData>(dst)) {
                b->height = a->height;
                b->style = a->style;
                b->widthFactor = a->widthFactor;
                b->oblique = a->oblique;
            }
        }
        if (const auto* a = entityAs<MTextData>(src)) {
            if (auto* b = entityAs<MTextData>(dst)) {
                b->height = a->height;
                b->style = a->style;
            }
        }
    }
    if (cats.polyline) {
        if (const auto* a = entityAs<PolylineData>(src)) {
            if (auto* b = entityAs<PolylineData>(dst)) {
                b->constantWidth = a->constantWidth;
                for (PolyVertex& v : b->v)
                    v.startWidth = v.endWidth = a->constantWidth;
            }
        }
    }
    if (cats.hatch) {
        if (const auto* a = entityAs<HatchData>(src)) {
            if (auto* b = entityAs<HatchData>(dst)) {
                b->solid = a->solid;
                b->pattern = a->pattern;
                b->patternScale = a->patternScale;
                b->patternAngle = a->patternAngle;
                b->doubled = a->doubled;
                b->fill = a->fill;
                b->gradient = a->gradient;
            }
        }
    }
    if (cats.dimension) {
        if (const auto* a = entityAs<DimensionData>(src))
            if (auto* b = entityAs<DimensionData>(dst))
                b->style = a->style;
        if (const auto* a = entityAs<LeaderData>(src))
            if (auto* b = entityAs<LeaderData>(dst))
                b->style = a->style;
        if (const auto* a = entityAs<MLeaderData>(src))
            if (auto* b = entityAs<MLeaderData>(dst)) {
                b->style = a->style;
                b->textHeight = a->textHeight;
            }
    }
}

// MATCHPROP Settings: switch the property groups on and off; the choice is remembered.
cmd::Task askMatchSettings(cmd::CommandContext& ctx) {
    for (;;) {
        MatchCats c = readMatchCats(ctx.host());
        std::string line = "Properties to match:";
        for (std::size_t i = 0; i < kMatchCats.size(); ++i)
            line += std::string(" ") + kMatchCats[i] + (matchFlag(c, i) ? "=On" : "=Off");
        ctx.message(line);
        cmd::KeywordOptions ko;
        for (const char* k : kMatchCats)
            ko.keywords.push_back(k);
        auto r = co_await ctx.getKeyword(
            "Toggle [Basic/Text/Dimension/Hatch/Polyline/Annotative] <Done>", std::move(ko));
        if (r.status != cmd::Status::Keyword)
            co_return;
        for (std::size_t i = 0; i < kMatchCats.size(); ++i)
            if (r.keyword == kMatchCats[i])
                putFlag(ctx.host(), std::string("matchprop.") + kMatchCats[i], !matchFlag(c, i));
    }
}

class MatchPropCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "MATCHPROP"; }
    std::vector<std::string> defaultAliases() const override { return {"MA"}; }
    std::string_view description() const override {
        return "Copy the properties of one object to other objects";
    }

    cmd::Task run(cmd::CommandContext& ctx) override {
        ctx.commitOnCancel();
        ctx.setUndoLabel("MATCHPROP");
        cmd::EntityOptions eo;
        eo.filter = [&doc = ctx.doc()](const Entity& e) { return doc.layer(e.common.layer); };
        auto src = co_await ctx.getEntity("Select source object", eo);
        if (!src.ok())
            co_return;
        std::size_t done = 0;
        for (;;) {
            cmd::EntityOptions to;
            to.filter = modifiableFilter(ctx.doc());
            to.keywords = {"Settings"};
            auto dst = co_await ctx.getEntity("Select destination object(s) or [Settings]", to);
            if (dst.status == cmd::Status::Keyword && dst.keyword == "Settings") {
                co_await askMatchSettings(ctx);
                continue;
            }
            if (!dst.ok())
                break;
            const Entity* s = ctx.doc().find(src.id);
            if (!s)
                break;
            if (dst.id == src.id)
                continue;
            Entity copy = *s;
            const MatchCats cats = readMatchCats(ctx.host());
            // An associative array draws the items of its own anonymous block; their own
            // properties win over the Insert's, so they take the new properties too.
            std::vector<EntityId> inner;
            if (const Entity* d = ctx.doc().find(dst.id))
                if (const auto* ins = entityAs<InsertData>(*d))
                    if (ins->array)
                        if (const BlockDef* def = ctx.doc().block(ins->block))
                            if (def->anonymous)
                                inner = def->entities;
            if (ctx.tx().modify(dst.id, [&](Entity& d) { matchProps(copy, d, cats); })) {
                for (EntityId id : inner)
                    (void)ctx.tx().modify(id, [&](Entity& d) { matchProps(copy, d, cats); });
                ++done;
            }
        }
        if (done)
            ctx.message(plural(done, "object", "objects") + " updated.");
    }
};

// ---- OVERKILL -----------------------------------------------------------------------------------

class OverkillCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "OVERKILL"; }
    std::string_view description() const override {
        return "Delete duplicate objects and merge overlapping lines and arcs";
    }

    cmd::Task run(cmd::CommandContext& ctx) override {
        cmd::SelectionOptions so;
        so.filter = editableFilter(ctx.doc());
        auto sel = co_await ctx.getSelection("Select objects to clean up", so);
        if (!sel.ok())
            co_return;
        modify::OverkillOptions oo;
        oo.tolerance = getSetting(ctx.host(), "overkill.tolerance", 1e-6);
        oo.ignoreProperties = getFlag(ctx.host(), "overkill.ignoreproperties", false);
        oo.combineOverlapping = getFlag(ctx.host(), "overkill.overlapping", true);
        oo.combineEndToEnd = getFlag(ctx.host(), "overkill.endtoend", true);
        std::vector<EntityId> ids;
        std::vector<const Entity*> entities;
        for (EntityId id : sel.ids) {
            if (const Entity* e = ctx.doc().find(id)) {
                ids.push_back(id);
                entities.push_back(e);
            }
        }
        modify::OverkillResult res = modify::overkill(entities, oo);
        std::vector<Entity> created = std::move(res.created);
        for (std::size_t i : res.erased) {
            if (auto r = ctx.tx().erase(ids[i]); !r)
                ctx.message(r.error().message);
        }
        for (Entity& e : created) {
            if (auto r = ctx.tx().add(std::move(e)); !r)
                ctx.message(r.error().message);
        }
        std::size_t merged = res.erased.size() - res.duplicates;
        if (res.erased.empty()) {
            ctx.message("No duplicate or overlapping objects found.");
        } else {
            ctx.message(plural(res.duplicates, "duplicate", "duplicates") + " deleted, " +
                        plural(merged, "object", "objects") + " merged into " +
                        std::to_string(created.size()) + ".");
        }
    }
};

// ---- PEDIT --------------------------------------------------------------------------------------

class PeditCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "PEDIT"; }
    std::vector<std::string> defaultAliases() const override { return {"PE"}; }
    std::string_view description() const override { return "Edit polylines and splines"; }

    cmd::Task run(cmd::CommandContext& ctx) override {
        ctx.commitOnCancel();
        ctx.setUndoLabel("PEDIT");
        cmd::EntityOptions eo;
        eo.filter = [&doc = ctx.doc()](const Entity& e) {
            if (!isEditable(doc, e))
                return false;
            auto k = kind(e);
            return k == EntityKind::Polyline || k == EntityKind::Line || k == EntityKind::Arc ||
                   k == EntityKind::Spline;
        };
        auto pick = co_await ctx.getEntity("Select polyline", eo);
        if (!pick.ok())
            co_return;
        EntityId id = pick.id;
        const Entity* e = ctx.doc().find(id);
        if (!e)
            co_return;
        if (kind(*e) == EntityKind::Line || kind(*e) == EntityKind::Arc) {
            cmd::KeywordOptions ko;
            ko.keywords = {"Yes", "No"};
            ko.defaultKeyword = "Yes";
            auto k = co_await ctx.getKeyword(
                "Object is not a polyline. Turn it into one? [Yes/No] <Yes>", ko);
            if (k.status == cmd::Status::Cancel || k.keyword != "Yes")
                co_return;
            e = ctx.doc().find(id);
            auto pl = e ? modify::toPolylineEntity(*e) : Result<Entity>(Error{});
            if (!pl) {
                ctx.message(pl.error().message);
                co_return;
            }
            if (auto r = replaceEntity(ctx.tx(), id, pl.value()); !r) {
                ctx.message(r.error().message);
                co_return;
            }
        }
        for (;;) {
            e = ctx.doc().find(id);
            if (!e)
                co_return;
            bool isSpline = kind(*e) == EntityKind::Spline;
            cmd::KeywordOptions ko;
            if (isSpline) {
                ko.keywords = {"Decurve", "Reverse", "Exit"};
            } else {
                const auto* pl = entityAs<PolylineData>(*e);
                ko.keywords = {pl && pl->closed ? "Open" : "Close",
                               "Join",
                               "Width",
                               "Spline",
                               "Vertices",
                               "Reverse",
                               "Exit"};
            }
            ko.defaultKeyword = "Exit";
            auto k = co_await ctx.getKeyword(
                isSpline
                    ? "Enter an option [Decurve/Reverse/Exit] <Exit>"
                    : "Enter an option [Close/Open/Join/Width/Spline/Vertices/Reverse/Exit] <Exit>",
                ko);
            if (!k.ok() || k.keyword == "Exit")
                co_return;
            if (k.keyword == "Close" || k.keyword == "Open") {
                Entity copy = *e;
                if (auto* pl = entityAs<PolylineData>(copy))
                    pl->closed = k.keyword == "Close";
                (void)replaceEntity(ctx.tx(), id, copy);
            } else if (k.keyword == "Width") {
                cmd::DistanceOptions d;
                d.defaultValue = 0;
                auto w = co_await ctx.getDistance("Specify new width for all segments", d);
                if (!w.ok())
                    continue;
                if (auto edit = modify::setPolylineWidth(*ctx.doc().find(id), w.value))
                    (void)applyEdit(ctx.tx(), id, std::move(edit.value()));
                else
                    ctx.message(edit.error().message);
            } else if (k.keyword == "Vertices") {
                for (;;) {
                    cmd::KeywordOptions vo;
                    vo.keywords = {"Insert", "Remove", "Move", "Exit"};
                    vo.defaultKeyword = "Exit";
                    auto v = co_await ctx.getKeyword(
                        "Vertex option [Insert/Remove/Move/Exit] <Exit>", vo);
                    if (!v.ok() || v.keyword == "Exit")
                        break;
                    auto at = co_await ctx.getPoint(v.keyword == "Insert"
                                                        ? "Specify a point on the segment to split"
                                                        : "Specify a point near the vertex");
                    if (!at.ok())
                        continue;
                    const Entity* cur = ctx.doc().find(id);
                    if (!cur)
                        co_return;
                    Result<modify::EntityEdit> edit =
                        Error{ErrorCode::InvalidArgument, "Unsupported"};
                    if (v.keyword == "Insert") {
                        edit = modify::insertPolylineVertex(*cur, at.point);
                    } else if (v.keyword == "Remove") {
                        edit = modify::removePolylineVertex(*cur, at.point);
                    } else {
                        cmd::PointOptions po;
                        auto to = co_await ctx.getPoint("Specify the new location", po);
                        if (!to.ok())
                            continue;
                        cur = ctx.doc().find(id);
                        if (!cur)
                            co_return;
                        edit = modify::movePolylineVertex(*cur, at.point, to.point);
                    }
                    if (!edit)
                        ctx.message(edit.error().message);
                    else
                        (void)applyEdit(ctx.tx(), id, std::move(edit.value()));
                }
            } else if (k.keyword == "Reverse") {
                if (auto edit = modify::reverseEntity(*e))
                    (void)applyEdit(ctx.tx(), id, std::move(edit.value()));
            } else if (k.keyword == "Spline") {
                auto sp = modify::polylineToSpline(*e);
                if (!sp) {
                    ctx.message(sp.error().message);
                    continue;
                }
                (void)replaceEntity(ctx.tx(), id, sp.value());
            } else if (k.keyword == "Decurve") {
                auto pl = modify::splineToPolyline(*e, 0.001);
                if (!pl) {
                    ctx.message(pl.error().message);
                    continue;
                }
                (void)replaceEntity(ctx.tx(), id, pl.value());
            } else if (k.keyword == "Join") {
                cmd::SelectionOptions so;
                so.filter = [&doc = ctx.doc(), id](const Entity& x) {
                    return x.common.id != id && isEditable(doc, x) &&
                           (kind(x) == EntityKind::Line || kind(x) == EntityKind::Arc ||
                            kind(x) == EntityKind::Polyline);
                };
                auto more = co_await ctx.getSelection("Select objects to join", so);
                if (!more.ok())
                    continue;
                std::vector<EntityId> ids{id};
                for (EntityId m : more.ids) {
                    if (m != id)
                        ids.push_back(m);
                }
                std::vector<const Entity*> es;
                for (EntityId m : ids)
                    es.push_back(ctx.doc().find(m));
                auto res = modify::joinEntities(es, getSetting(ctx.host(), "join.tolerance", 1e-3));
                if (res.consumed.empty()) {
                    ctx.message("Nothing could be joined.");
                    continue;
                }
                std::vector<Entity> created = std::move(res.created);
                for (std::size_t c : res.consumed)
                    (void)ctx.tx().erase(ids[c]);
                if (created.size() == 1) {
                    auto added = ctx.tx().add(std::move(created[0]));
                    if (added)
                        id = added.value();
                } else {
                    for (Entity& n : created)
                        (void)ctx.tx().add(std::move(n));
                    co_return;
                }
            }
        }
    }
};

}  // namespace

std::unique_ptr<cmd::Command> makeLengthen() {
    return std::make_unique<LengthenCommand>();
}
std::unique_ptr<cmd::Command> makeReverse() {
    return std::make_unique<ReverseCommand>();
}
std::unique_ptr<cmd::Command> makeDivide() {
    return std::make_unique<DivideCommand>();
}
std::unique_ptr<cmd::Command> makeMeasure() {
    return std::make_unique<MeasureCommand>();
}
std::unique_ptr<cmd::Command> makeAlign() {
    return std::make_unique<AlignCommand>();
}
std::unique_ptr<cmd::Command> makeMatchProp() {
    return std::make_unique<MatchPropCommand>();
}
std::unique_ptr<cmd::Command> makeOverkill() {
    return std::make_unique<OverkillCommand>();
}
std::unique_ptr<cmd::Command> makePedit() {
    return std::make_unique<PeditCommand>();
}

}  // namespace tbcad::modifycmd
