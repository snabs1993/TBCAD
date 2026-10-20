#include <algorithm>
#include <array>
#include <cmath>
#include <unordered_set>

#include "commands.h"
#include "tbcad/blocks/tbcad_blocks.h"
#include "tbcad/transform/apply.h"
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
        // Like AutoCAD: until a mode is chosen a pick only measures the object.
        std::string mode;
        for (;;) {
            cmd::EntityOptions eo;
            eo.keywords = {"DElta", "Percent", "Total", "DYnamic"};
            if (!mode.empty())
                eo.keywords.push_back("Undo");
            eo.filter = [&doc = ctx.doc()](const Entity& e) {
                return selectable(doc, e) && modify::lengthenableLength(e).has_value();
            };
            auto pick = co_await ctx.getEntity(
                mode.empty() ? "Select an object to measure or [DElta/Percent/Total/DYnamic]"
                             : "Select an object to change or [DElta/Percent/Total/DYnamic/Undo]",
                eo);
            if (pick.status == cmd::Status::Keyword) {
                if (pick.keyword == "Undo") {
                    ctx.message(ctx.rollbackToCheckpoint() ? "Last change undone."
                                                           : "Nothing to undo.");
                    continue;
                }
                const std::string chosen = pick.keyword == "DElta"     ? "Delta"
                                           : pick.keyword == "DYnamic" ? "Dynamic"
                                                                       : pick.keyword;
                if (chosen == "Delta" || chosen == "Total") {
                    // A length, or [Angle] for arcs.
                    const bool delta = chosen == "Delta";
                    cmd::DistanceOptions d;
                    d.allowNegative = delta;
                    d.allowZero = delta;
                    d.keywords = {"Angle"};
                    d.defaultValue = delta ? delta_ : (total_ > 0 ? total_ : 1.0);
                    auto r = co_await ctx.getDistance(
                        delta ? "Enter delta length or [Angle]" : "Specify total length or [Angle]",
                        d);
                    if (r.status == cmd::Status::Keyword) {
                        // Degrees; a negative delta shortens.
                        cmd::RealOptions ao;
                        if (!delta)
                            ao.min = 1e-9;
                        auto a = co_await ctx.getReal(
                            delta ? "Enter delta angle" : "Specify total angle", ao);
                        if (!a.ok())
                            co_return;
                        angleMode_ = true;
                        (delta ? deltaAngle_ : totalAngle_) = a.value * kPi / 180.0;
                    } else if (r.ok()) {
                        angleMode_ = false;
                        (delta ? delta_ : total_) = r.value;
                        if (delta)
                            putSetting(ctx.host(), "lengthen.delta", r.value);
                    } else {
                        co_return;
                    }
                } else if (chosen == "Percent") {
                    cmd::RealOptions ro;
                    ro.defaultValue = getSetting(ctx.host(), "lengthen.percent", 100);
                    ro.min = 0.001;
                    auto r = co_await ctx.getReal("Enter percentage length", ro);
                    if (!r.ok())
                        co_return;
                    putSetting(ctx.host(), "lengthen.percent", r.value);
                    percent_ = r.value;
                }
                mode = chosen;
                continue;
            }
            if (!pick.ok())
                co_return;
            const Entity* e = ctx.doc().find(pick.id);
            if (!e)
                continue;
            if (mode.empty()) {
                ctx.message(measure(*e));
                continue;
            }
            const Entity source = *e;
            const Vec2 at = pick.pickPoint;
            const auto* arc = entityAs<ArcData>(source);
            Result<modify::EntityEdit> edit = Error{ErrorCode::InvalidArgument, "Unsupported"};
            if ((mode == "Delta" || mode == "Total") && angleMode_) {
                if (!arc) {
                    ctx.message("Angles change arcs only.");
                    continue;
                }
                // An angle on an arc is a length along it: radius x radians.
                edit = mode == "Delta" ? modify::lengthenEntity(source, at, LengthenMode::Delta,
                                                                arc->radius * deltaAngle_)
                                       : modify::lengthenEntity(source, at, LengthenMode::Total,
                                                                arc->radius * totalAngle_);
            } else if (mode == "Delta") {
                edit = modify::lengthenEntity(source, at, LengthenMode::Delta, delta_);
            } else if (mode == "Percent") {
                edit = modify::lengthenEntity(source, at, LengthenMode::Percent, percent_);
            } else if (mode == "Total") {
                edit = modify::lengthenEntity(source, at, LengthenMode::Total, total_);
            } else {
                cmd::PointOptions po;
                po.preview = [source, at, &doc = ctx.doc(), &host = ctx.host()](Vec2 cursor,
                                                                                DisplayList& out) {
                    auto r = modify::lengthenToPoint(source, at, cursor);
                    if (r && r.value().modified)
                        previewEntity(doc, host, *r.value().modified, out);
                };
                auto p = co_await ctx.getPoint("Specify new end point", po);
                if (!p.ok())
                    continue;
                edit = modify::lengthenToPoint(source, at, p.point);
            }
            if (!edit) {
                ctx.message(edit.error().message);
                continue;
            }
            ctx.checkpoint();
            if (auto ids = applyEdit(ctx.tx(), pick.id, std::move(edit.value())); !ids)
                ctx.message(ids.error().message);
        }
    }

private:
    // "Current length: 12.5" and, for an arc, its included angle.
    static std::string measure(const Entity& e) {
        char buf[96];
        const double len = modify::lengthenableLength(e).value_or(0);
        if (const auto* a = entityAs<ArcData>(e)) {
            double sweep = std::fmod(a->endAngle - a->startAngle, 2 * kPi);
            if (sweep <= 0)
                sweep += 2 * kPi;
            std::snprintf(buf, sizeof buf, "Current length: %.4f, included angle: %.4f", len,
                          sweep * 180.0 / kPi);
        } else {
            std::snprintf(buf, sizeof buf, "Current length: %.4f", len);
        }
        return buf;
    }

    double delta_ = 10, percent_ = 100, total_ = 0;
    double deltaAngle_ = 0, totalAngle_ = 0;  // radians
    bool angleMode_ = false;                  // DElta / Total were given an Angle
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
        const std::vector<Vec2> ends = geom::endpoints(*e);
        if (ends.size() >= 2)
            fromEnd =
                distance(pick.pickPoint, ends.back()) < distance(pick.pickPoint, ends.front());
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
        // Live preview: the selection as it would sit with the cursor as the destination.
        const std::vector<EntityId>& picked = sel.ids;
        auto show = [&ctx, &picked](const Transform2& t, DisplayList& out) {
            transform::buildPreview(ctx.doc(), picked, t, ctx.host().view().scale, out);
        };
        cmd::PointOptions po1;
        po1.base = s1.point;
        po1.preview = [show, a = s1.point](Vec2 c, DisplayList& out) {
            show(Transform2::translate(c - a), out);
        };
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
            po3.preview = [show, a = s1.point, b = d1.point, c2 = s2.point](Vec2 c,
                                                                            DisplayList& out) {
                auto t = modify::alignTransform(a, b, c2, c, false);
                if (t)
                    show(t.value(), out);
            };
            auto d2 = co_await ctx.getPoint("Specify second destination point", po3);
            if (!d2.ok())
                co_return;
            src2 = s2.point;
            dst2 = d2.point;
            // As AutoCAD: a third pair (which only matters in 3D) or Enter to continue. Yes or No
            // here answers the scale question at once.
            cmd::PointOptions po4;
            po4.allowNone = true;
            po4.keywords = {"Yes", "No"};
            auto s3 = co_await ctx.getPoint("Specify third source point or <continue>", po4);
            if (s3.status == cmd::Status::Cancel)
                co_return;
            std::string answer;
            if (s3.status == cmd::Status::Keyword) {
                answer = s3.keyword;
            } else if (s3.ok()) {
                cmd::PointOptions po5;
                po5.base = s3.point;
                auto d3 = co_await ctx.getPoint("Specify third destination point", po5);
                if (!d3.ok())
                    co_return;
                ctx.message("A drawing is flat: the third pair of points is not needed.");
            }
            if (answer.empty()) {
                cmd::KeywordOptions ko;
                ko.keywords = {"Yes", "No"};
                ko.defaultKeyword = "No";
                auto k = co_await ctx.getKeyword(
                    "Scale objects based on alignment points? [Yes/No] <No>", ko);
                if (k.status == cmd::Status::Cancel)
                    co_return;
                answer = k.keyword;
            }
            scale = answer == "Yes";
        } else if (s2.status == cmd::Status::Cancel) {
            co_return;
        }
        auto t = modify::alignTransform(s1.point, d1.point, src2, dst2, scale);
        if (!t) {
            ctx.message(t.error().message);
            co_return;
        }
        const transform::ApplyReport rep =
            transform::applyTransform(ctx, sel.ids, t.value(), false);
        if (!rep.firstError.empty())
            ctx.message(rep.firstError);
        const std::size_t done = rep.applied;
        ctx.message(plural(done, "object", "objects") + " aligned.");
    }
};

// ---- MATCHPROP ----------------------------------------------------------------------------------

// The properties MATCHPROP passes on, as in AutoCAD's Property Settings: each basic property on
// its own, then the special groups. Every one can be switched off with Settings; the choice is
// remembered under "matchprop.<name>".
enum MatchCat : std::size_t {
    kMColor,
    kMLayer,  // the level goes with the layer
    kMLinetype,
    kMLtScale,
    kMLineweight,
    kMTransparency,
    kMPlotStyle,
    kMText,       // text and MText height and style; text width, slant and justification
    kMDimension,  // dimension and leader style
    kMHatch,      // pattern, scale, angle, fill
    kMPolyline,   // width
    kMTable,      // text style, text height and cell styles
    kMMleader,    // multileader style and text height
    kMAnnotative,
    kMatchCount
};

struct MatchCatName {
    const char* key;      // setting name
    const char* keyword;  // what the Settings prompt takes (capitals: the shortest input)
};
constexpr std::array<MatchCatName, kMatchCount> kMatchCats = {{
    {"Color", "Color"},
    {"Layer", "LAyer"},
    {"Linetype", "LType"},
    {"LinetypeScale", "LTScale"},
    {"Lineweight", "LWeight"},
    {"Transparency", "TRansparency"},
    {"PlotStyle", "PStyle"},
    {"Text", "Text"},
    {"Dimension", "Dimension"},
    {"Hatch", "Hatch"},
    {"Polyline", "POlyline"},
    {"Table", "TAble"},
    {"Multileader", "Multileader"},
    {"Annotative", "ANnotative"},
}};
constexpr std::size_t kBasicCount = kMPlotStyle + 1;  // Basic switches these together

using MatchCats = std::array<bool, kMatchCount>;

MatchCats readMatchCats(const cmd::IHost& host) {
    MatchCats c{};
    // Files from before the basic properties were split kept one "Basic" switch.
    const bool basic = getFlag(host, "matchprop.Basic", true);
    for (std::size_t i = 0; i < kMatchCount; ++i)
        c[i] = getFlag(host, std::string("matchprop.") + kMatchCats[i].key,
                       i < kBasicCount ? basic : true);
    return c;
}

MatchCats allMatchCats() {
    MatchCats c{};
    c.fill(true);
    return c;
}

void matchProps(const Entity& src, Entity& dst, const MatchCats& cats = allMatchCats()) {
    if (cats[kMLayer]) {
        dst.common.layer = src.common.layer;
        dst.common.level = src.common.level;
    }
    if (cats[kMColor])
        dst.common.color = src.common.color;
    if (cats[kMLinetype])
        dst.common.linetype = src.common.linetype;
    if (cats[kMLtScale])
        dst.common.linetypeScale = src.common.linetypeScale;
    if (cats[kMLineweight])
        dst.common.lineweight = src.common.lineweight;
    if (cats[kMTransparency])
        dst.common.transparency = src.common.transparency;
    if (cats[kMAnnotative]) {
        dst.common.annotative = src.common.annotative;
        dst.common.annoScales = src.common.annoScales;
    }
    if (cats[kMText]) {
        const auto* ta = entityAs<TextData>(src);
        const auto* ma = entityAs<MTextData>(src);
        if (ta) {
            if (auto* b = entityAs<TextData>(dst)) {
                b->height = ta->height;
                b->style = ta->style;
                b->widthFactor = ta->widthFactor;
                b->oblique = ta->oblique;
                // Justification: the point the text hangs from stays where it is. Aligned and Fit
                // need two points of their own, so they are not passed on or replaced.
                const auto twoPoint = [](HAlign h) {
                    return h == HAlign::Aligned || h == HAlign::Fit;
                };
                if (!twoPoint(ta->h) && !twoPoint(b->h) && (ta->h != b->h || ta->v != b->v)) {
                    const auto leftBase = [](const TextData& t) {
                        return t.h == HAlign::Left && t.v == VAlign::Baseline;
                    };
                    const Vec2 anchor = leftBase(*b) ? b->position : b->alignPoint;
                    b->h = ta->h;
                    b->v = ta->v;
                    b->position = anchor;
                    b->alignPoint = anchor;
                }
            }
        }
        // Text and MText pass their height and style to each other, as in AutoCAD.
        const double height = ta ? ta->height : ma ? ma->height : 0;
        const TextStyleId style = ta ? ta->style : ma ? ma->style : TextStyleId{};
        if (ta || ma) {
            if (auto* b = entityAs<MTextData>(dst)) {
                b->height = height;
                b->style = style;
            } else if (auto* t = entityAs<TextData>(dst); t && ma) {
                t->height = height;
                t->style = style;
            }
        }
    }
    if (cats[kMPolyline]) {
        if (const auto* a = entityAs<PolylineData>(src)) {
            if (auto* b = entityAs<PolylineData>(dst)) {
                b->constantWidth = a->constantWidth;
                for (PolyVertex& v : b->v)
                    v.startWidth = v.endWidth = a->constantWidth;
            }
        }
    }
    if (cats[kMHatch]) {
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
    if (cats[kMDimension]) {
        if (const auto* a = entityAs<DimensionData>(src))
            if (auto* b = entityAs<DimensionData>(dst))
                b->style = a->style;
        if (const auto* a = entityAs<LeaderData>(src))
            if (auto* b = entityAs<LeaderData>(dst))
                b->style = a->style;
    }
    if (cats[kMMleader]) {
        if (const auto* a = entityAs<MLeaderData>(src))
            if (auto* b = entityAs<MLeaderData>(dst)) {
                b->style = a->style;
                b->textHeight = a->textHeight;
            }
    }
    if (cats[kMTable]) {
        if (const auto* a = entityAs<TableData>(src))
            if (auto* b = entityAs<TableData>(dst)) {
                b->style = a->style;
                b->textHeight = a->textHeight;
                b->margin = a->margin;
                b->titleStyle = a->titleStyle;
                b->headerStyle = a->headerStyle;
                b->dataStyle = a->dataStyle;
            }
    }
}

// MATCHPROP Settings: switch properties on and off (Basic switches the seven basic ones
// together); the choice is remembered. Enter ends.
cmd::Task askMatchSettings(cmd::CommandContext& ctx) {
    for (;;) {
        const MatchCats c = readMatchCats(ctx.host());
        std::string line = "Properties to match:";
        for (std::size_t i = 0; i < kMatchCount; ++i)
            line += std::string(" ") + kMatchCats[i].key + (c[i] ? "=On" : "=Off");
        ctx.message(line);
        cmd::KeywordOptions ko;
        std::string prompt = "Basic";
        ko.keywords.push_back("Basic");
        for (const MatchCatName& m : kMatchCats) {
            ko.keywords.push_back(m.keyword);
            prompt += std::string("/") + m.keyword;
        }
        auto r = co_await ctx.getKeyword("Toggle [" + prompt + "] <Done>", std::move(ko));
        if (r.status != cmd::Status::Keyword)
            co_return;
        if (r.keyword == "Basic") {
            bool anyOn = false;
            for (std::size_t i = 0; i < kBasicCount; ++i)
                anyOn = anyOn || c[i];
            for (std::size_t i = 0; i < kBasicCount; ++i)
                putFlag(ctx.host(), std::string("matchprop.") + kMatchCats[i].key, !anyOn);
            continue;
        }
        for (std::size_t i = 0; i < kMatchCount; ++i)
            if (r.keyword == kMatchCats[i].keyword)
                putFlag(ctx.host(), std::string("matchprop.") + kMatchCats[i].key, !c[i]);
    }
}

class MatchPropCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "MATCHPROP"; }
    std::vector<std::string> defaultAliases() const override { return {"MA", "PAINTER", "MATCH"}; }
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
            // As in AutoCAD each pick, window or crossing is matched at once; Enter ends.
            cmd::SelectionOptions to;
            to.filter = modifiableFilter(ctx.doc());
            to.keywords = {"Settings"};
            to.allowPreselection = false;
            to.single = true;
            auto dst = co_await ctx.getSelection("Select destination object(s) or [Settings]", to);
            if (dst.status == cmd::Status::Keyword && dst.keyword == "Settings") {
                co_await askMatchSettings(ctx);
                continue;
            }
            if (!dst.ok() || dst.ids.empty())
                break;
            const Entity* s = ctx.doc().find(src.id);
            if (!s)
                break;
            Entity copy = *s;
            const MatchCats cats = readMatchCats(ctx.host());
            for (EntityId target : dst.ids)
                if (target != src.id && matchOne(ctx, copy, src.id, target, cats))
                    ++done;
        }
        if (done)
            ctx.message(plural(done, "object", "objects") + " updated.");
    }

private:
    static bool matchOne(cmd::CommandContext& ctx, const Entity& copy, EntityId srcId,
                         EntityId target, const MatchCats& cats) {
        // An associative array draws the items of its own anonymous block; their own
        // properties win over the Insert's, so they take the new properties too.
        std::vector<EntityId> inner;
        if (const Entity* d = ctx.doc().find(target))
            if (const auto* ins = entityAs<InsertData>(*d))
                if (ins->array)
                    if (const BlockDef* def = ctx.doc().block(ins->block))
                        if (def->anonymous)
                            inner = def->entities;
        if (!ctx.tx().modify(target, [&](Entity& d) { matchProps(copy, d, cats); }))
            return false;
        for (EntityId id : inner)
            (void)ctx.tx().modify(id, [&](Entity& d) { matchProps(copy, d, cats); });
        if (cats[kMPlotStyle]) {  // the plot style name lives in the header, not on the object
            const std::string style = entityPlotStyle(ctx.doc().header(), srcId);
            if (style != entityPlotStyle(ctx.doc().header(), target))
                (void)ctx.tx().modifyHeader([&](Header& h) {
                    setEntityPlotStyle(h, target, style);
                    for (EntityId id : inner)
                        setEntityPlotStyle(h, id, style);
                });
        }
        return true;
    }
};

// ---- OVERKILL -----------------------------------------------------------------------------------

class OverkillCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "OVERKILL"; }
    std::vector<std::string> defaultAliases() const override { return {"-OVERKILL"}; }
    std::string_view description() const override {
        return "Delete duplicate objects and merge overlapping lines and arcs";
    }

    cmd::Task run(cmd::CommandContext& ctx) override {
        cmd::SelectionOptions so;
        so.filter = editableFilter(ctx.doc());
        so.keywords = {"Options"};
        auto sel = co_await ctx.getSelection("Select objects to clean up or [Options]", so);
        while (sel.status == cmd::Status::Keyword) {
            co_await options(ctx);
            sel = co_await ctx.getSelection("Select objects to clean up or [Options]", so);
        }
        if (!sel.ok())
            co_return;
        modify::OverkillOptions oo;
        oo.tolerance = getSetting(ctx.host(), "overkill.tolerance", 1e-6);
        oo.ignoreProperties = getFlag(ctx.host(), "overkill.ignoreproperties", false);
        oo.combineOverlapping = getFlag(ctx.host(), "overkill.overlapping", true);
        oo.combineEndToEnd = getFlag(ctx.host(), "overkill.endtoend", true);
        oo.optimizePolylines = getFlag(ctx.host(), "overkill.optimize", false);
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
        std::size_t merged = res.erased.size() - res.duplicates - res.optimized;
        if (res.erased.empty()) {
            ctx.message("No duplicate or overlapping objects found.");
        } else {
            std::string msg = plural(res.duplicates, "duplicate", "duplicates") + " deleted, " +
                              plural(merged, "object", "objects") + " merged into " +
                              std::to_string(created.size() - res.optimized) + ".";
            if (res.optimized)
                msg += " " + plural(res.optimized, "polyline", "polylines") + " optimized.";
            ctx.message(msg);
        }
    }

private:
    static cmd::Task options(cmd::CommandContext& ctx);
};

// The settings OVERKILL keeps (as AutoCAD's -OVERKILL options): how close counts as the same,
// whether colour/layer/linetype... are ignored, and which merges happen.
cmd::Task OverkillCommand::options(cmd::CommandContext& ctx) {
    auto& host = ctx.host();
    for (;;) {
        auto yn = [](bool on) { return std::string(on ? "Yes" : "No"); };
        const std::string state =
            "Tolerance " + formatDouble(getSetting(host, "overkill.tolerance", 1e-6)) +
            ", ignore properties " + yn(getFlag(host, "overkill.ignoreproperties", false)) +
            ", optimize polylines " + yn(getFlag(host, "overkill.optimize", false)) +
            ", combine overlapping " + yn(getFlag(host, "overkill.overlapping", true)) +
            ", combine end to end " + yn(getFlag(host, "overkill.endtoend", true));
        ctx.message(state);
        cmd::KeywordOptions ko;
        ko.keywords = {"Done", "Tolerance", "Ignore", "Plines", "parTial", "Endtoend"};
        ko.defaultKeyword = "Done";
        auto k = co_await ctx.getKeyword(
            "Option to change [Done/Tolerance/Ignore/Plines/parTial overlap/Endtoend] <Done>", ko);
        if (!k.ok() || k.keyword == "Done")
            co_return;
        if (k.keyword == "Tolerance") {
            cmd::RealOptions ro;
            ro.min = 0;
            ro.defaultValue = getSetting(host, "overkill.tolerance", 1e-6);
            auto r = co_await ctx.getReal("Tolerance (how close counts as the same)", ro);
            if (r.ok())
                putSetting(host, "overkill.tolerance", r.value);
            continue;
        }
        const char* key = k.keyword == "Ignore"    ? "overkill.ignoreproperties"
                          : k.keyword == "Plines"  ? "overkill.optimize"
                          : k.keyword == "parTial" ? "overkill.overlapping"
                                                   : "overkill.endtoend";
        const std::string question =
            k.keyword == "Ignore"    ? "Ignore colour, layer, linetype and other properties"
            : k.keyword == "Plines"  ? "Optimize segments within polylines"
            : k.keyword == "parTial" ? "Combine collinear objects that partially overlap"
                                     : "Combine collinear objects that meet end to end";
        const bool fallback = k.keyword == "parTial" || k.keyword == "Endtoend";
        cmd::KeywordOptions yo;
        yo.keywords = {"Yes", "No"};
        yo.defaultKeyword = getFlag(host, key, fallback) ? "Yes" : "No";
        auto a = co_await ctx.getKeyword(question, yo);
        if (a.ok())
            putFlag(host, key, a.keyword == "Yes");
    }
}

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
        eo.keywords = {"Multiple"};
        auto pick = co_await ctx.getEntity("Select polyline or [Multiple]", eo);
        if (pick.status == cmd::Status::Keyword && pick.keyword == "Multiple") {
            co_await editMultiple(ctx, eo.filter);
            co_return;
        }
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
                ko.keywords = {"Decurve", "Tolerance", "Reverse", "Undo", "Exit"};
            } else {
                const auto* pl = entityAs<PolylineData>(*e);
                ko.keywords = {pl && pl->closed ? "Open" : "Close",
                               "Join",
                               "Fuzz",
                               "Width",
                               "Taper",
                               "Fit",
                               "Spline",
                               "Decurve",
                               "Ltypegen",
                               "Vertices",
                               "Reverse",
                               "Undo",
                               "Exit"};
            }
            ko.defaultKeyword = "Exit";
            auto k = co_await ctx.getKeyword(
                isSpline ? "Enter an option [Decurve/Tolerance/Reverse/Undo/Exit] <Exit>"
                         : "Enter an option "
                           "[Close/Open/Join/Fuzz/Width/Taper/Fit/Spline/Decurve/"
                           "Ltypegen/Vertices/Reverse/Undo/Exit] <Exit>",
                ko);
            if (!k.ok() || k.keyword == "Exit")
                co_return;
            if (k.keyword == "Undo") {
                // Takes back the last option, like AutoCAD's PEDIT Undo.
                if (undoIds_.empty() || !ctx.rollbackToCheckpoint()) {
                    ctx.message("Nothing to undo.");
                } else {
                    id = undoIds_.back();
                    undoIds_.pop_back();
                }
                continue;
            }
            if (k.keyword != "Fuzz") {
                ctx.checkpoint();
                undoIds_.push_back(id);
            }
            if (!isSpline && k.keyword == "Decurve") {
                // Straight segments between the vertices again (undoes Fit's arcs).
                Entity copy = *e;
                if (auto* pl = entityAs<PolylineData>(copy)) {
                    for (PolyVertex& v : pl->v)
                        v.bulge = 0;
                    (void)replaceEntity(ctx.tx(), id, copy);
                }
                continue;
            }
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
            } else if (k.keyword == "Taper") {
                cmd::DistanceOptions d;
                d.defaultValue = 0;
                auto w0 = co_await ctx.getDistance("Specify the starting width", d);
                if (!w0.ok())
                    continue;
                d.defaultValue = w0.value;
                auto w1 = co_await ctx.getDistance("Specify the ending width", d);
                if (!w1.ok())
                    continue;
                if (auto edit = modify::taperPolyline(*ctx.doc().find(id), w0.value, w1.value))
                    (void)applyEdit(ctx.tx(), id, std::move(edit.value()));
                else
                    ctx.message(edit.error().message);
            } else if (k.keyword == "Vertices") {
                for (;;) {
                    cmd::KeywordOptions vo;
                    vo.keywords = {"Insert", "Remove", "Move", "Width", "Straighten", "Exit"};
                    vo.defaultKeyword = "Exit";
                    auto v = co_await ctx.getKeyword(
                        "Vertex option [Insert/Remove/Move/Width/Straighten/Exit] <Exit>", vo);
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
                    if (v.keyword == "Width") {
                        cmd::DistanceOptions d;
                        d.defaultValue = 0;
                        auto w0 = co_await ctx.getDistance("Specify starting width", d);
                        if (!w0.ok())
                            continue;
                        d.defaultValue = w0.value;
                        auto w1 = co_await ctx.getDistance("Specify ending width", d);
                        if (!w1.ok())
                            continue;
                        cur = ctx.doc().find(id);
                        if (!cur)
                            co_return;
                        edit = modify::setPolylineVertexWidth(*cur, at.point, w0.value, w1.value);
                    } else if (v.keyword == "Straighten") {
                        auto to = co_await ctx.getPoint("Specify the other end vertex");
                        if (!to.ok())
                            continue;
                        cur = ctx.doc().find(id);
                        if (!cur)
                            co_return;
                        edit = modify::straightenPolyline(*cur, at.point, to.point);
                    } else if (v.keyword == "Insert") {
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
            } else if (k.keyword == "Fit") {
                auto fit = modify::fitPolylineCurve(*e);
                if (!fit)
                    ctx.message(fit.error().message);
                else
                    (void)replaceEntity(ctx.tx(), id, fit.value());
            } else if (k.keyword == "Ltypegen") {
                cmd::KeywordOptions lo;
                lo.keywords = {"ON", "OFF"};
                lo.defaultKeyword = "ON";
                auto l = co_await ctx.getKeyword(
                    "Full PLINE linetype generation [ON/OFF] <ON> (OFF restarts the pattern at "
                    "each vertex)",
                    lo);
                if (l.ok()) {
                    Entity copy = *ctx.doc().find(id);
                    if (auto* pl = entityAs<PolylineData>(copy)) {
                        pl->restartLinetype = l.keyword == "OFF";
                        (void)replaceEntity(ctx.tx(), id, copy);
                    }
                }
            } else if (k.keyword == "Fuzz") {
                co_await askFuzz(ctx);
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
            } else if (k.keyword == "Tolerance") {
                cmd::RealOptions ro;
                ro.defaultValue = 0.1;
                ro.min = 1e-6;
                auto tolerance = co_await ctx.getReal(
                    "Specify how far the new curve may stray from the old one <0.1>", ro);
                if (!tolerance.ok())
                    continue;
                auto edit = modify::refitSpline(*e, tolerance.value);
                if (!edit) {
                    ctx.message(edit.error().message);
                    continue;
                }
                (void)applyEdit(ctx.tx(), id, std::move(edit.value()));
                if (const auto* ns = entityAs<SplineData>(*ctx.doc().find(id)))
                    ctx.message("Fitted again through " + std::to_string(ns->fit.size()) +
                                " points.");
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
                auto res = modify::joinEntities(
                    es, fuzz_ >= 0 ? fuzz_ : getSetting(ctx.host(), "join.tolerance", 1e-3), false,
                    jointype(ctx.host()));
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

private:
    double fuzz_ = -1;               // negative = the join.tolerance setting
    std::vector<EntityId> undoIds_;  // the polyline's id before each option, for Undo

    // PEDIT Jointype, kept in the pedit.jointype setting: how Join closes a gap within the fuzz
    // distance. Extend (default, as in AutoCAD), Add or Both.
    static modify::JoinGap jointype(const cmd::IHost& host) {
        const std::string t = host.setting("pedit.jointype").value_or("extend");
        return t == "add"    ? modify::JoinGap::Add
               : t == "both" ? modify::JoinGap::Both
                             : modify::JoinGap::Extend;
    }

    // "Specify the fuzz distance for Join or [Jointype]".
    cmd::Task askFuzz(cmd::CommandContext& ctx) {
        cmd::DistanceOptions d;
        d.defaultValue = fuzz_ >= 0 ? fuzz_ : getSetting(ctx.host(), "join.tolerance", 1e-3);
        d.keywords = {"Jointype"};
        auto f = co_await ctx.getDistance("Specify the fuzz distance for Join or [Jointype]", d);
        if (f.status == cmd::Status::Keyword) {
            cmd::KeywordOptions ko;
            ko.keywords = {"Extend", "Add", "Both"};
            const auto cur = jointype(ctx.host());
            ko.defaultKeyword = cur == modify::JoinGap::Add    ? "Add"
                                : cur == modify::JoinGap::Both ? "Both"
                                                               : "Extend";
            auto k = co_await ctx.getKeyword("Enter join type [Extend/Add/Both]", std::move(ko));
            if (!k.ok())
                co_return;
            std::string v = k.keyword;
            for (char& c : v)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            ctx.host().setSetting("pedit.jointype", v);
            co_return;
        }
        if (f.ok() && f.value >= 0)
            fuzz_ = f.value;
    }

    // PEDIT Multiple: the options act on every selected object.
    cmd::Task editMultiple(cmd::CommandContext& ctx, cmd::EntityFilterFn filter) {
        cmd::SelectionOptions so;
        so.filter = std::move(filter);
        auto sel = co_await ctx.getSelection("Select objects", so);
        if (!sel.ok() || sel.ids.empty())
            co_return;
        std::vector<EntityId> ids = sel.ids;
        // Lines and arcs become polylines first, like the single-object command's "Yes".
        std::vector<EntityId> lone;
        for (EntityId id : ids) {
            const Entity* e = ctx.doc().find(id);
            if (e && (kind(*e) == EntityKind::Line || kind(*e) == EntityKind::Arc))
                lone.push_back(id);
        }
        if (!lone.empty()) {
            cmd::KeywordOptions ko;
            ko.keywords = {"Yes", "No"};
            ko.defaultKeyword = "Yes";
            auto k = co_await ctx.getKeyword(
                "Some objects are not polylines. Turn them into polylines? [Yes/No] <Yes>", ko);
            if (k.status == cmd::Status::Cancel)
                co_return;
            const bool convert = k.keyword == "Yes";
            std::vector<EntityId> kept;
            const std::unordered_set<EntityId> loneSet(lone.begin(), lone.end());
            for (EntityId id : ids) {
                const Entity* e = ctx.doc().find(id);
                if (!e)
                    continue;
                if (!loneSet.count(id)) {
                    kept.push_back(id);
                } else if (convert) {
                    auto pl = modify::toPolylineEntity(*e);
                    if (pl && replaceEntity(ctx.tx(), id, pl.value()))
                        kept.push_back(id);
                }
            }
            ids = std::move(kept);
        }
        for (;;) {
            cmd::KeywordOptions ko;
            ko.keywords = {"Close",  "Open",    "Join",     "Fuzz",    "Width", "Fit",
                           "Spline", "Decurve", "Ltypegen", "Reverse", "Exit"};
            ko.defaultKeyword = "Exit";
            auto k = co_await ctx.getKeyword(
                "Enter an option "
                "[Close/Open/Join/Fuzz/Width/Fit/Spline/Decurve/Ltypegen/Reverse/Exit] <Exit>",
                ko);
            if (!k.ok() || k.keyword == "Exit")
                co_return;
            std::size_t changed = 0;
            if (k.keyword == "Fuzz") {
                co_await askFuzz(ctx);
            } else if (k.keyword == "Join") {
                std::vector<const Entity*> es;
                for (EntityId id : ids)
                    es.push_back(ctx.doc().find(id));
                auto res = modify::joinEntities(
                    es, fuzz_ >= 0 ? fuzz_ : getSetting(ctx.host(), "join.tolerance", 1e-3), false,
                    jointype(ctx.host()));
                if (res.consumed.empty()) {
                    ctx.message("Nothing could be joined.");
                    continue;
                }
                std::vector<Entity> created = std::move(res.created);
                std::vector<EntityId> gone;
                for (std::size_t c : res.consumed)
                    gone.push_back(ids[c]);
                for (EntityId g : gone)
                    (void)ctx.tx().erase(g);
                const std::unordered_set<EntityId> goneSet(gone.begin(), gone.end());
                std::erase_if(ids, [&](EntityId i) { return goneSet.count(i) != 0; });
                changed = gone.size();
                for (Entity& n : created) {
                    if (auto added = ctx.tx().add(std::move(n)))
                        ids.push_back(added.value());
                }
            } else {
                double width = 0;
                bool restartPattern = false;
                if (k.keyword == "Ltypegen") {
                    cmd::KeywordOptions lo;
                    lo.keywords = {"ON", "OFF"};
                    lo.defaultKeyword = "ON";
                    auto l =
                        co_await ctx.getKeyword("Full PLINE linetype generation [ON/OFF] <ON>", lo);
                    if (!l.ok())
                        continue;
                    restartPattern = l.keyword == "OFF";
                }
                if (k.keyword == "Width") {
                    cmd::DistanceOptions d;
                    d.defaultValue = 0;
                    auto w = co_await ctx.getDistance("Specify new width for all segments", d);
                    if (!w.ok())
                        continue;
                    width = w.value;
                }
                for (EntityId id : ids) {
                    const Entity* e = ctx.doc().find(id);
                    if (!e)
                        continue;
                    const bool spline = kind(*e) == EntityKind::Spline;
                    Result<modify::EntityEdit> edit = Error{ErrorCode::InvalidArgument, "n/a"};
                    if (k.keyword == "Close" || k.keyword == "Open") {
                        Entity copy = *e;
                        auto* pl = entityAs<PolylineData>(copy);
                        if (!pl || pl->closed == (k.keyword == "Close"))
                            continue;
                        pl->closed = k.keyword == "Close";
                        if (replaceEntity(ctx.tx(), id, copy))
                            ++changed;
                        continue;
                    }
                    if (k.keyword == "Width") {
                        if (spline)
                            continue;
                        edit = modify::setPolylineWidth(*e, width);
                    } else if (k.keyword == "Reverse") {
                        edit = modify::reverseEntity(*e);
                    } else if (k.keyword == "Fit") {
                        auto fit = modify::fitPolylineCurve(*e);
                        if (fit && replaceEntity(ctx.tx(), id, fit.value()))
                            ++changed;
                        continue;
                    } else if (k.keyword == "Ltypegen") {
                        Entity copy = *e;
                        auto* pl = entityAs<PolylineData>(copy);
                        if (pl && pl->restartLinetype != restartPattern &&
                            (pl->restartLinetype = restartPattern, true) &&
                            replaceEntity(ctx.tx(), id, copy))
                            ++changed;
                        continue;
                    } else if (k.keyword == "Spline" || k.keyword == "Decurve") {
                        const bool toSpline = k.keyword == "Spline";
                        if (toSpline == spline)
                            continue;
                        auto conv = toSpline ? modify::polylineToSpline(*e)
                                             : modify::splineToPolyline(*e, 0.001);
                        if (conv && replaceEntity(ctx.tx(), id, conv.value()))
                            ++changed;
                        continue;
                    }
                    if (edit && applyEdit(ctx.tx(), id, std::move(edit.value())))
                        ++changed;
                }
            }
            ctx.message(std::to_string(changed) + (changed == 1 ? " object" : " objects") +
                        " changed.");
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
