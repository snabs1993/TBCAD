#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <optional>

#include "commands.h"
#include "tbcad/blocks/tbcad_blocks.h"
#include "tbcad/geom/curve.h"

namespace tbcad::block_cmds {

namespace {

constexpr std::size_t kPreviewCopies = 400;  // the preview stops here; the result is not limited

// Draws copies of `source` at the given transforms (at most kPreviewCopies).
void previewCopies(const Document& doc, const std::vector<Entity>& source,
                   const std::vector<Transform2>& ts, double pixelSize, DisplayList& out) {
    geom::DisplayContext dc;
    dc.pixelSizeMm = pixelSize;
    std::size_t n = 0;
    for (const Transform2& t : ts) {
        if (++n > kPreviewCopies)
            break;
        for (const Entity& e : source)
            if (auto moved = geom::transformed(e, t))
                geom::buildDisplay(doc, moved.value(), dc, out);
    }
}

// Curve geometry of the path object as polyline vertices; empty when the kind is not supported.
bool pathVertices(const Entity& e, std::vector<PolyVertex>& v, bool& closed) {
    closed = false;
    if (const auto* l = entityAs<LineData>(e)) {
        v = {PolyVertex{l->a}, PolyVertex{l->b}};
        return true;
    }
    if (const auto* pl = entityAs<PolylineData>(e)) {
        v = pl->v;
        closed = pl->closed;
        return v.size() >= 2;
    }
    if (const auto* a = entityAs<ArcData>(e)) {
        double sweep = normalizeAngle(a->endAngle - a->startAngle);
        if (sweep <= 0)
            sweep = kTwoPi;
        Vec2 p0 = a->center + Vec2{std::cos(a->startAngle), std::sin(a->startAngle)} * a->radius;
        Vec2 p1 =
            a->center +
            Vec2{std::cos(a->startAngle + sweep), std::sin(a->startAngle + sweep)} * a->radius;
        v = {PolyVertex{p0, std::tan(sweep / 4)}, PolyVertex{p1}};
        return true;
    }
    if (const auto* c = entityAs<CircleData>(e)) {
        Vec2 p0 = c->center + Vec2{c->radius, 0};
        Vec2 p1 = c->center - Vec2{c->radius, 0};
        v = {PolyVertex{p0, 1}, PolyVertex{p1, 1}};
        closed = true;
        return true;
    }
    // Ellipses and splines: a fine polyline (a few hundredths of a millimetre on a 1 m curve), so
    // the items sit on the curve and turn with it.
    std::optional<geom::CurveRef> curve;
    if (const auto* el = entityAs<EllipseData>(e)) {
        double sweep = el->endParam - el->startParam;
        if (sweep <= 0)
            sweep += kTwoPi;
        closed = std::abs(sweep - kTwoPi) < 1e-9;
        curve = geom::CurveRef::ellipse(el->center, el->majorAxis, el->ratio, el->startParam, sweep);
    } else if (const auto* sp = entityAs<SplineData>(e)) {
        auto c = geom::CurveRef::spline(*sp);
        if (!c)
            return false;
        curve = c.value();
        closed = sp->closed;
    }
    if (!curve)
        return false;
    const double size = std::max(curve->length(), 1e-9);
    std::vector<Vec2> pts;
    geom::flatten(*curve, std::max(size * 2e-5, 1e-6), pts);
    if (closed && pts.size() > 2 && distance(pts.front(), pts.back()) < 1e-9)
        pts.pop_back();
    if (pts.size() < 2)
        return false;
    v.clear();
    for (Vec2 q : pts)
        v.push_back(PolyVertex{q});
    return true;
}

// A number for prompts: at most two decimals, no trailing zeros, never 2506.265664160396.
std::string niceNumber(double v) {
    char buf[48];
    std::snprintf(buf, sizeof buf, "%.2f", v);
    std::string s = buf;
    if (s.find('.') != std::string::npos) {
        while (s.back() == '0')
            s.pop_back();
        if (s.back() == '.')
            s.pop_back();
    }
    return s == "-0" ? "0" : s;
}

// Default spacing for an object of the given extent: 1.5 x the extent, rounded to two
// significant digits (so 141.4 gives 210 and 36.7 gives 55).
double niceSpacing(double extent) {
    const double v = extent * 1.5;
    const double mag = std::pow(10.0, std::floor(std::log10(v)) - 1);
    return std::round(v / mag) * mag;
}

std::string freeArrayName(const Document& doc) {
    for (int i = 1;; ++i) {
        std::string n = "*A" + std::to_string(i);
        bool used = false;
        for (const auto& b : doc.blocks())
            used = used || b.second.name == n;
        if (!used)
            return n;
    }
}

// ARRAY [AR] and ARRAYRECT / ARRAYPOLAR / ARRAYPATH: rectangular, polar or path patterns of the
// selected objects, either plain copies or one associative array (an Insert of an anonymous
// block whose parameters ARRAYEDIT can change).
class ArrayCommand final : public cmd::Command {
public:
    enum class Mode { Any, Rect, Polar, Path };
    explicit ArrayCommand(Mode m = Mode::Any) : mode_(m) {}

    std::string_view name() const override {
        switch (mode_) {
            case Mode::Rect:
                return "ARRAYRECT";
            case Mode::Polar:
                return "ARRAYPOLAR";
            case Mode::Path:
                return "ARRAYPATH";
            default:
                return "ARRAY";
        }
    }
    std::vector<std::string> defaultAliases() const override {
        return mode_ == Mode::Any ? std::vector<std::string>{"AR"} : std::vector<std::string>{};
    }
    std::string_view description() const override {
        switch (mode_) {
            case Mode::Rect:
                return "Associative rectangular array";
            case Mode::Polar:
                return "Associative polar array";
            case Mode::Path:
                return "Associative array along a path";
            default:
                return "Copy objects in a rectangular, polar or path pattern";
        }
    }

    cmd::Task run(cmd::CommandContext& ctx) override {
        Document& doc = ctx.doc();
        auto sel =
            co_await ctx.getSelection("Select objects to array",
                                      cmd::SelectionOptions{.filter = detail::editableFilter(doc)});
        if (!sel.ok() || sel.ids.empty())
            co_return;
        std::vector<Entity> source;
        for (EntityId id : detail::inDrawOrder(doc, sel.ids))
            source.push_back(*doc.find(id));
        if (source.empty())
            co_return;
        const BBox2 box = detail::boundsOf(doc, source);

        std::string typeName = "Rectangular";
        if (mode_ == Mode::Polar) {
            typeName = "Polar";
        } else if (mode_ == Mode::Path) {
            typeName = "Path";
        } else if (mode_ == Mode::Any) {
            auto type = co_await ctx.getKeyword(
                "Array type [Rectangular/Polar/Path] <Rectangular>",
                cmd::KeywordOptions{.keywords = {"Rectangular", "Polar", "Path"},
                                    .defaultKeyword = "Rectangular"});
            if (!type.ok())
                co_return;
            typeName = type.keyword;
        }

        ArrayInfo info;
        std::vector<Entity> copies;
        bool associative = true;
        bool ok = false;
        if (typeName == "Path")
            co_await pathSetup(ctx, sel.ids, source, box, info, copies, associative, ok);
        else if (typeName == "Polar")
            co_await polarSetup(ctx, sel.ids, source, box, info, copies, associative, ok);
        else
            co_await rectSetup(ctx, sel.ids, source, box, info, copies, associative, ok);
        if (!ok)
            co_return;
        if (typeName == "Path")
            co_return;  // a path array is always associative and already made

        if (associative) {
            createAssociative(ctx, sel.ids, info);
            co_return;
        }

        std::vector<EntityId> created;
        for (Entity& e : copies) {
            auto added = ctx.tx().add(std::move(e));
            if (!added) {
                ctx.message(added.error().message);
                co_return;  // rolls the whole array back
            }
            created.push_back(added.value());
        }
        ctx.host().setSelection(created);
        ctx.message("Created " + detail::objects(created.size()));
        ctx.setUndoLabel("Array " + detail::objects(created.size()));
    }

private:
    // Path array setup. `ok` is true when the user finished the setup; a cancel leaves it false.
    static cmd::Task pathSetup(cmd::CommandContext& ctx, const std::vector<EntityId>& ids,
                               const std::vector<Entity>& source, const BBox2& box, ArrayInfo& info,
                               std::vector<Entity>& copies, bool& associative, bool& ok) {
        Document& doc = ctx.doc();
        (void)ids;
        (void)copies;
        (void)associative;
        (void)box;
        (void)source;
        (void)doc;
        cmd::EntityOptions eo;
        eo.filter = [](const Entity& e) {
            std::vector<PolyVertex> v;
            bool closed = false;
            return pathVertices(e, v, closed);
        };
        auto picked =
            co_await ctx.getEntity("Select path curve (line, arc, circle, polyline, ellipse or spline)", eo);
        if (!picked.ok())
            co_return;
        info.kind = ArrayInfo::Kind::Path;
        pathVertices(*doc.find(picked.id), info.path, info.pathClosed);
        PolylineData pl;
        pl.v = info.path;
        pl.closed = info.pathClosed;
        double total = 0;
        for (const auto& g : geom::segments(pl))
            total += g.curve.length();
        if (!(total > 0)) {
            ctx.message("The path has no length");
            co_return;
        }
        const Vec2 centre = box.empty() ? Vec2{} : box.center();
        auto base = co_await ctx.getPoint(
            "Specify base point (the point of the objects placed on the path) <centre>");
        if (base.status == cmd::Status::Cancel)
            co_return;
        info.reference = base.ok() ? base.point : centre;
        auto method = co_await ctx.getKeyword(
            "Method [Count/Distance] <Count>",
            cmd::KeywordOptions{.keywords = {"Count", "Distance"}, .defaultKeyword = "Count"});
        if (!method.ok())
            co_return;
        if (method.keyword == "Distance") {
            auto d = co_await ctx.getDistance("Specify the distance between items along the path",
                                              cmd::DistanceOptions{.allowZero = false});
            if (!d.ok())
                co_return;
            info.pathSpacing = d.value;
            info.count = static_cast<int>(
                std::min<double>(blocks::kMaxArrayItems,
                                 std::floor(total / d.value + 1e-9) + (info.pathClosed ? 0 : 1)));
        } else {
            auto n = co_await ctx.getInteger(
                "Enter the number of items <6>",
                cmd::IntOptions{.defaultValue = 6, .min = 1, .max = blocks::kMaxArrayItems});
            if (!n.ok())
                co_return;
            info.count = static_cast<int>(n.value);
        }
        auto align = co_await ctx.getKeyword(
            "Align items to the path? [Yes/No] <Yes>",
            cmd::KeywordOptions{.keywords = {"Yes", "No"}, .defaultKeyword = "Yes"});
        if (!align.ok())
            co_return;
        info.rotateItems = align.keyword == "Yes";
        createAssociative(ctx, ids, info);
        co_return;
        ok = true;
    }

    // Polar array setup. `ok` is true when the user finished the setup; a cancel leaves it false.
    static cmd::Task polarSetup(cmd::CommandContext& ctx, const std::vector<EntityId>& ids,
                                const std::vector<Entity>& source, const BBox2& box,
                                ArrayInfo& info, std::vector<Entity>& copies, bool& associative,
                                bool& ok) {
        Document& doc = ctx.doc();
        (void)ids;
        (void)copies;
        (void)associative;
        (void)box;
        (void)source;
        (void)doc;
        auto center = co_await ctx.getPoint("Specify center point of array");
        if (!center.ok())
            co_return;
        const Vec2 c = center.point;
        int n = 6;
        double fillAngle = kTwoPi;
        bool rotate = true;
        for (;;) {
            auto opt = co_await ctx.getKeyword(
                "Polar array of " + std::to_string(n) + " items over " +
                    niceNumber(fillAngle * 180 / kPi) +
                    " degrees [Items/Angle/Rotate/Associative] <Accept>",
                cmd::KeywordOptions{
                    .keywords = {"Accept", "Items", "Angle", "Rotate", "Associative"},
                    .defaultKeyword = "Accept"});
            if (!opt.ok())
                co_return;
            if (opt.keyword == "Accept")
                break;
            if (opt.keyword == "Items") {
                auto v = co_await ctx.getInteger(
                    "Enter the number of items in the array (including the original) <" +
                        std::to_string(n) + ">",
                    cmd::IntOptions{.defaultValue = n, .min = 2, .max = blocks::kMaxArrayItems});
                if (!v.ok())
                    co_return;
                n = static_cast<int>(v.value);
            } else if (opt.keyword == "Angle") {
                const double pixel = ctx.host().view().scale;
                const int items = n;
                auto preview = [&doc, source, c, items, pixel](Vec2 cursor, DisplayList& out) {
                    blocks::PolarArray spec{c, items, 0, true, std::nullopt};
                    double a = std::atan2(cursor.y - c.y, cursor.x - c.x);
                    spec.fillAngle = a > 0 ? a : a + kTwoPi;
                    previewCopies(
                        doc, source,
                        blocks::polarTransforms(spec, detail::boundsOf(doc, source).center()),
                        pixel, out);
                };
                auto fill = co_await ctx.getAngle(
                    "Specify the angle to fill (+ = counterclockwise, - = clockwise) <" +
                        niceNumber(fillAngle * 180 / kPi) + ">",
                    cmd::AngleOptions{.base = c, .preview = preview, .defaultValue = fillAngle});
                if (!fill.ok())
                    co_return;
                fillAngle = fill.value;
            } else if (opt.keyword == "Rotate") {
                rotate = !rotate;
                ctx.message(rotate ? "Items rotate as copied" : "Items keep their direction");
            } else {
                associative = !associative;
                ctx.message(associative ? "The array is associative"
                                        : "The array is made of plain copies");
            }
        }
        blocks::PolarArray spec{c, n, fillAngle, rotate, std::nullopt};
        info.kind = ArrayInfo::Kind::Polar;
        info.center = c;
        info.count = n;
        info.fillAngle = fillAngle;
        info.rotateItems = rotate;
        info.reference = box.empty() ? Vec2{} : box.center();
        auto r = blocks::arrayPolar(source, spec);
        if (!r) {
            ctx.message(r.error().message);
            co_return;
        }
        copies = std::move(r).value();
        ok = true;
    }

    // Rectangular array setup. `ok` is true when the user finished the setup; a cancel leaves it
    // false.
    static cmd::Task rectSetup(cmd::CommandContext& ctx, const std::vector<EntityId>& ids,
                               const std::vector<Entity>& source, const BBox2& box, ArrayInfo& info,
                               std::vector<Entity>& copies, bool& associative, bool& ok) {
        Document& doc = ctx.doc();
        (void)ids;
        (void)copies;
        (void)associative;
        (void)box;
        (void)source;
        (void)doc;
        blocks::RectArray spec;
        spec.angle = ctx.host().ucsAngle();  // rows run along the UCS X axis
        spec.rows = 3;
        spec.columns = 4;
        spec.columnSpacing = box.empty() || box.width() <= 0 ? 10.0 : niceSpacing(box.width());
        spec.rowSpacing = box.empty() || box.height() <= 0 ? 10.0 : niceSpacing(box.height());
        const double pixel = ctx.host().view().scale;
        const Vec2 anchor = box.empty() ? Vec2{} : box.min;
        for (;;) {
            auto opt = co_await ctx.getKeyword(
                std::to_string(spec.rows) + " rows x " + std::to_string(spec.columns) +
                    " columns, spacing " + niceNumber(spec.columnSpacing) + " x " +
                    niceNumber(spec.rowSpacing) + ", angle " + niceNumber(spec.angle * 180 / kPi) +
                    " [Rows/Columns/Spacing/Angle/Associative] <Accept>",
                cmd::KeywordOptions{
                    .keywords = {"Accept", "Rows", "Columns", "Spacing", "Angle", "Associative"},
                    .defaultKeyword = "Accept"});
            if (!opt.ok())
                co_return;
            if (opt.keyword == "Accept")
                break;
            if (opt.keyword == "Rows" || opt.keyword == "Columns") {
                const bool rows = opt.keyword == "Rows";
                const int other = rows ? spec.columns : spec.rows;
                auto preview = [&doc, source, spec, rows, other, pixel](const std::string& typed,
                                                                        DisplayList& out) {
                    const int k = std::atoi(typed.c_str());
                    if (k < 1 || k > blocks::kMaxArrayItems)
                        return;
                    blocks::RectArray s = spec;
                    s.rows = rows ? k : other;
                    s.columns = rows ? other : k;
                    previewCopies(doc, source, blocks::rectangularTransforms(s), pixel, out);
                };
                const int cur = rows ? spec.rows : spec.columns;
                auto v = co_await ctx.getInteger(std::string("Enter the number of ") +
                                                     (rows ? "rows" : "columns") + " <" +
                                                     std::to_string(cur) + ">",
                                                 cmd::IntOptions{.defaultValue = cur,
                                                                 .min = 1,
                                                                 .max = blocks::kMaxArrayItems,
                                                                 .textPreview = preview});
                if (!v.ok())
                    co_return;
                (rows ? spec.rows : spec.columns) = static_cast<int>(v.value);
            } else if (opt.keyword == "Spacing") {
                auto colSp = co_await ctx.getDistance(
                    "Specify the distance between columns <" + niceNumber(spec.columnSpacing) + ">",
                    cmd::DistanceOptions{.defaultValue = spec.columnSpacing,
                                         .allowZero = false,
                                         .allowNegative = true});
                if (!colSp.ok())
                    co_return;
                spec.columnSpacing = colSp.value;
                auto preview = [&doc, source, spec, anchor, pixel](Vec2 cursor, DisplayList& out) {
                    blocks::RectArray s = spec;
                    s.rowSpacing = distance(cursor, anchor);
                    previewCopies(doc, source, blocks::rectangularTransforms(s), pixel, out);
                };
                auto rowSp = co_await ctx.getDistance(
                    "Specify the distance between rows <" + niceNumber(spec.rowSpacing) + ">",
                    cmd::DistanceOptions{.base = anchor,
                                         .preview = preview,
                                         .defaultValue = spec.rowSpacing,
                                         .allowZero = false,
                                         .allowNegative = true});
                if (!rowSp.ok())
                    co_return;
                spec.rowSpacing = rowSp.value;
            } else if (opt.keyword == "Angle") {
                auto ang = co_await ctx.getAngle(
                    "Specify the angle of the array <" + niceNumber(spec.angle * 180 / kPi) + ">",
                    cmd::AngleOptions{.defaultValue = spec.angle - ctx.host().ucsAngle(),
                                      .inUcs = true});
                if (!ang.ok())
                    co_return;
                spec.angle = ang.value;
            } else {
                associative = !associative;
                ctx.message(associative ? "The array is associative"
                                        : "The array is made of plain copies");
            }
        }
        info.kind = ArrayInfo::Kind::Rectangular;
        info.rows = spec.rows;
        info.columns = spec.columns;
        info.rowSpacing = spec.rowSpacing;
        info.columnSpacing = spec.columnSpacing;
        info.angle = spec.angle;
        auto r = blocks::arrayRectangular(source, spec);
        if (!r) {
            ctx.message(r.error().message);
            co_return;
        }
        copies = std::move(r).value();
        ok = true;
    }

    // Moves the objects into a new anonymous block and puts one Insert showing it per item.
    static void createAssociative(cmd::CommandContext& ctx, std::vector<EntityId> ids,
                                  ArrayInfo info) {
        Document& doc = ctx.doc();
        std::vector<Entity> content;
        for (EntityId id : detail::inDrawOrder(doc, ids))
            content.push_back(*doc.find(id));
        Transaction& tx = ctx.tx();
        auto block = tx.addBlock(BlockDef{.name = freeArrayName(doc), .anonymous = true});
        if (!block) {
            ctx.message(block.error().message);
            return;
        }
        for (Entity& e : content) {
            e.common.id = {};
            if (auto r = tx.add(std::move(e), block.value()); !r) {
                ctx.message(r.error().message);
                return;  // the transaction is rolled back when the command ends
            }
        }
        for (EntityId id : ids)
            (void)tx.erase(id);
        InsertData data{block.value(), {}, {1, 1}, 0};
        const long long items = blocks::arrayItemCount(info);
        data.array = std::move(info);
        Entity ins;
        ins.data = Indirect<InsertData>(std::move(data));
        auto r = tx.add(std::move(ins));
        if (!r) {
            ctx.message(r.error().message);
            return;
        }
        ctx.host().setSelection({r.value()});
        ctx.message("Created an associative array of " + std::to_string(items) + " items");
        ctx.setUndoLabel("Array " + std::to_string(items) + " items");
    }

    Mode mode_;
};

// Associative arrays near `p` in the drawing being edited (the host's own search when it has one).
std::vector<EntityId> arraysNear(cmd::CommandContext& ctx, Vec2 p, double tol) {
    const Document& doc = ctx.doc();
    BBox2 window;
    window.expand(p);
    window = window.inflated(std::max(tol, 1e-9));
    std::vector<EntityId> out;
    auto consider = [&](const Entity& e) {
        const auto* ins = entityAs<InsertData>(e);
        if (ins && ins->array && blocks::insertBounds(doc, e).inflated(tol).contains(p) &&
            detail::editable(doc, e))
            out.push_back(e.common.id);
    };
    if (auto ids = ctx.host().entitiesInWindow(window)) {
        for (EntityId id : *ids)
            if (const Entity* e = doc.find(id))
                consider(*e);
        return out;
    }
    const BlockId owner = ctx.host().drawingOwner();
    doc.forEachEntity([&](const Entity& e) {
        if (doc.ownerBlock(e.common.id) == owner)
            consider(e);
    });
    return out;
}

// ARRAYREMOVE: takes items out of associative arrays (Ctrl+click on an item starts it). Each pick
// removes the item under it; Enter ends. One undo step; an array with nothing left goes.
class ArrayRemoveCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "ARRAYREMOVE"; }
    std::string_view description() const override {
        return "Remove single items from an associative array (Ctrl+click an item)";
    }

    cmd::Task run(cmd::CommandContext& ctx) override {
        Document& doc = ctx.doc();
        int removed = 0;
        for (;;) {
            auto pt = co_await ctx.getPoint("Select an array item to remove",
                                            cmd::PointOptions{.allowNone = true, .snap = false});
            if (pt.status == cmd::Status::None)
                break;
            if (!pt.ok())
                co_return;
            bool done = false;
            for (EntityId id : arraysNear(ctx, pt.point, 0)) {
                const Entity* e = doc.find(id);
                const auto* ins = e ? entityAs<InsertData>(*e) : nullptr;
                if (!ins)
                    continue;
                auto item = blocks::arrayItemAt(doc, *ins, pt.point, 0);
                if (!item)
                    continue;
                ArrayInfo a = *ins->array;
                if (!blocks::removeArrayItem(a, *item))
                    continue;
                const bool empty =
                    a.swaps.empty() &&
                    static_cast<long long>(a.removed.size()) >= blocks::arrayItemCount(a);
                if (empty) {
                    (void)ctx.tx().erase(id);
                    ctx.message("That was the last item: the array was erased.");
                } else {
                    (void)ctx.tx().modify(id, [&](Entity& ent) {
                        if (auto* d = entityAs<InsertData>(ent))
                            d->array = a;
                    });
                }
                ++removed;
                done = true;
                break;
            }
            if (!done)
                ctx.message("No array item there.");
        }
        if (removed > 0) {
            ctx.message("Removed " + std::to_string(removed) +
                        (removed == 1 ? " array item." : " array items."));
            ctx.setUndoLabel("Remove array items");
        }
    }
};

// ARRAYEDIT: change the parameters of an associative array, one option at a time as in AutoCAD
// (Enter or eXit ends). Rectangular: Rows, Columns (count, then spacing or Total). Polar: Items,
// Angle between, Fill angle, ROTate items. Path: Method (Divide or Measure), Items, Align items.
// All kinds: Source (edit the source objects in place, as BEDIT; BSAVE or BCLOSE updates every
// item), REPlace (show chosen items as other objects) and RESet (bring back removed and replaced
// items). Every change shows at once and the whole edit is one undo step.
class ArrayEditCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "ARRAYEDIT"; }
    std::string_view description() const override {
        return "Change the rows, columns, spacing or count of an associative array";
    }

    cmd::Task run(cmd::CommandContext& ctx) override {
        Document& doc = ctx.doc();
        cmd::EntityOptions eo;
        eo.filter = [](const Entity& e) {
            const auto* ins = entityAs<InsertData>(e);
            return ins && ins->array;
        };
        auto picked = co_await ctx.getEntity("Select an associative array", eo);
        if (!picked.ok())
            co_return;
        const Entity* e = doc.find(picked.id);
        if (!e)
            co_return;
        ArrayInfo a = *entityAs<InsertData>(*e)->array;
        auto apply = [&] {
            (void)ctx.tx().modify(picked.id, [&](Entity& ent) {
                if (auto* d = entityAs<InsertData>(ent))
                    d->array = a;
            });
        };
        auto tooMany = [&](const ArrayInfo& x) {
            if (blocks::arrayItemCount(x) <= blocks::kMaxArrayItems)
                return false;
            ctx.message("That array would have too many items");
            return true;
        };
        auto yesNo = [](bool b) { return std::string(b ? "Yes" : "No"); };
        // The array as it is now (with `a`), for finding items.
        auto current = [&] {
            InsertData d = *entityAs<InsertData>(*doc.find(picked.id));
            d.array = a;
            return d;
        };

        for (;;) {
            cmd::KeywordOptions ko;
            std::string prompt;
            switch (a.kind) {
                case ArrayInfo::Kind::Rectangular:
                    ko.keywords = {"Source", "REPlace", "Rows", "Columns", "RESet", "eXit"};
                    break;
                case ArrayInfo::Kind::Polar:
                    ko.keywords = {"Source", "REPlace", "Items", "Angle",
                                   "Fill",   "ROTate",  "RESet", "eXit"};
                    break;
                case ArrayInfo::Kind::Path:
                    ko.keywords = {"Source", "REPlace", "Method", "Items", "Align", "RESet", "eXit"};
                    break;
            }
            for (const std::string& k : ko.keywords)
                prompt += (prompt.empty() ? "" : "/") + k;
            ko.defaultKeyword = "eXit";
            auto opt = co_await ctx.getKeyword("Enter an option [" + prompt + "] <eXit>", ko);
            if (!opt.ok())
                co_return;
            const std::string& k = opt.keyword;
            if (k == "eXit")
                break;
            const ArrayInfo before = a;

            if (k == "Source") {
                // Edited in place at the first item that shows the source objects.
                const InsertData d = current();
                const BlockDef* def = doc.block(d.block);
                if (!def) {
                    ctx.message("The source objects of this array no longer exist.");
                    continue;
                }
                const auto items = blocks::arrayItemTransforms(a);
                std::size_t first = 0;
                while (first < items.size() &&
                       !blocks::arrayItemShowsSource(a, static_cast<int>(first)))
                    ++first;
                Transform2 xf = blocks::instanceTransform(d, def->base);
                if (first < items.size())
                    xf = xf * items[first];
                if (!detail::startBlockEdit(ctx, *def, xf))
                    continue;
                co_return;
            }
            if (k == "RESet") {
                if (a.removed.empty() && a.swaps.empty()) {
                    ctx.message("No items were removed or replaced.");
                    continue;
                }
                const std::size_t n = a.removed.size() + a.swaps.size();
                a.removed.clear();
                a.swaps.clear();
                apply();
                ctx.message("Brought back " + std::to_string(n) + (n == 1 ? " item." : " items."));
                continue;
            }
            if (k == "REPlace") {
                auto sel = co_await ctx.getSelection(
                    "Select replacement objects",
                    cmd::SelectionOptions{.filter = detail::editableFilter(doc),
                                          .allowPreselection = false});
                if (!sel.ok())
                    co_return;
                std::vector<Entity> objs;
                for (EntityId id : detail::inDrawOrder(doc, sel.ids))
                    if (id != picked.id)
                        objs.push_back(*doc.find(id));
                if (objs.empty()) {
                    ctx.message("No replacement objects selected.");
                    continue;
                }
                const Vec2 centroid = detail::boundsOf(doc, objs).center();
                auto basePt = co_await ctx.getPoint(
                    "Select base point of replacement objects <centroid>",
                    cmd::PointOptions{.allowNone = true});
                if (basePt.status != cmd::Status::None && !basePt.ok())
                    co_return;
                const Vec2 base = basePt.ok() ? basePt.point : centroid;
                // The replacement block holds the objects in the array's own scale and rotation,
                // so they look as selected when an item shows them.
                const InsertData d = current();
                const BlockDef* srcDef = doc.block(d.block);
                Transform2 lin = blocks::instanceTransform(d, srcDef ? srcDef->base : Vec2{});
                lin.tx = lin.ty = 0;
                auto inv = lin.inverse();
                if (!inv) {
                    ctx.message("This array is scaled to nothing.");
                    continue;
                }
                const Transform2 toBlock = inv.value() * Transform2::translate(-base);
                Transaction& tx = ctx.tx();
                auto block = tx.addBlock(BlockDef{.name = freeArrayName(doc), .anonymous = true});
                if (!block) {
                    ctx.message(block.error().message);
                    co_return;
                }
                bool failed = false;
                for (const Entity& e : objs) {
                    auto moved = geom::transformed(e, toBlock);
                    if (!moved) {
                        ctx.message(moved.error().message);
                        failed = true;
                        break;
                    }
                    Entity copy = std::move(moved.value());
                    copy.common.id = {};
                    if (auto r = tx.add(std::move(copy), block.value()); !r) {
                        ctx.message(r.error().message);
                        failed = true;
                        break;
                    }
                }
                if (failed) {
                    (void)tx.eraseBlock(block.value());
                    continue;
                }
                const Vec2 at = blocks::arrayItemBase(doc, d);
                int replaced = 0;
                for (;;) {
                    cmd::PointOptions po{.allowNone = true, .snap = false};
                    po.keywords = {"Source"};
                    auto item = co_await ctx.getPoint(
                        "Select an item in the array to replace or [Source objects]", po);
                    if (item.status == cmd::Status::None)
                        break;
                    if (item.status == cmd::Status::Keyword) {
                        // every item that still shows the source objects
                        const long long n = blocks::arrayItemCount(a);
                        for (long long i = 0; i < n; ++i)
                            if (blocks::arrayItemShowsSource(a, static_cast<int>(i)) &&
                                blocks::swapArrayItem(a, {static_cast<int>(i), block.value(), at}))
                                ++replaced;
                        apply();
                        break;
                    }
                    if (!item.ok())
                        co_return;
                    auto hit = blocks::arrayItemAt(doc, current(), item.point, 0);
                    if (!hit) {
                        ctx.message("No array item there.");
                        continue;
                    }
                    if (blocks::swapArrayItem(a, {*hit, block.value(), at})) {
                        ++replaced;
                        apply();
                    }
                }
                if (replaced == 0) {
                    (void)tx.eraseBlock(block.value());
                    ctx.message("No items were replaced.");
                } else {
                    ctx.message("Replaced " + std::to_string(replaced) +
                                (replaced == 1 ? " item." : " items."));
                }
                continue;
            }

            if (k == "Rows" || k == "Columns") {
                const bool rows = k == "Rows";
                int& count = rows ? a.rows : a.columns;
                double& spacing = rows ? a.rowSpacing : a.columnSpacing;
                auto n = co_await ctx.getInteger(
                    std::string("Enter the number of ") + (rows ? "rows" : "columns") + " <" +
                        std::to_string(count) + ">",
                    cmd::IntOptions{.defaultValue = count, .min = 1,
                                    .max = blocks::kMaxArrayItems});
                if (!n.ok())
                    co_return;
                cmd::DistanceOptions dso{.defaultValue = spacing, .allowZero = false,
                                         .allowNegative = true};
                dso.keywords = {"Total"};
                auto sp = co_await ctx.getDistance(
                    std::string("Specify the distance between ") + (rows ? "rows" : "columns") +
                        " or [Total] <" + niceNumber(spacing) + ">",
                    dso);
                double newSpacing = sp.value;
                if (sp.status == cmd::Status::Keyword) {
                    // the distance from the first to the last row or column
                    auto total = co_await ctx.getDistance(
                        std::string("Specify the distance between the first and last ") +
                            (rows ? "row" : "column"),
                        cmd::DistanceOptions{.allowZero = false, .allowNegative = true});
                    if (!total.ok())
                        co_return;
                    if (n.value < 2) {
                        ctx.message("A total needs at least two rows or columns.");
                        continue;
                    }
                    newSpacing = total.value / static_cast<double>(n.value - 1);
                } else if (!sp.ok()) {
                    co_return;
                }
                ArrayInfo next = a;
                (rows ? next.rows : next.columns) = static_cast<int>(n.value);
                (rows ? next.rowSpacing : next.columnSpacing) = newSpacing;
                if (tooMany(next))
                    continue;
                a = next;
            } else if (k == "Items") {
                cmd::IntOptions io{.defaultValue = a.count, .min = 1,
                                   .max = blocks::kMaxArrayItems};
                if (a.kind == ArrayInfo::Kind::Path && a.pathSpacing > 0) {
                    // Measure: the spacing first, then how many fit (as AutoCAD asks)
                    auto sp = co_await ctx.getDistance(
                        "Specify the distance between items along the path <" +
                            niceNumber(a.pathSpacing) + ">",
                        cmd::DistanceOptions{.defaultValue = a.pathSpacing, .allowZero = false});
                    if (!sp.ok())
                        co_return;
                    a.pathSpacing = sp.value;
                }
                auto n = co_await ctx.getInteger(
                    "Enter the number of items <" + std::to_string(a.count) + ">", io);
                if (!n.ok())
                    co_return;
                a.count = static_cast<int>(n.value);
            } else if (k == "Angle") {  // angle between items
                const double step = a.count > 1 ? a.fillAngle / (a.count - 1) : a.fillAngle;
                auto ang = co_await ctx.getAngle(
                    "Specify the angle between items <" +
                        niceNumber(step * 180.0 / kPi) + ">",
                    cmd::AngleOptions{.defaultValue = step});
                if (!ang.ok())
                    co_return;
                if (a.count < 2 || std::abs(ang.value) < 1e-12) {
                    ctx.message("The angle between items needs two items and a non-zero angle.");
                    continue;
                }
                const double fill = ang.value * (a.count - 1);
                if (std::abs(fill) > kTwoPi + 1e-9) {
                    ctx.message("That many items at that angle would go round more than once.");
                    continue;
                }
                a.fillAngle = fill;
            } else if (k == "Fill") {
                auto fill = co_await ctx.getAngle(
                    "Specify the angle to fill (+ counter-clockwise, - clockwise) <" +
                        niceNumber(a.fillAngle * 180.0 / kPi) + ">",
                    cmd::AngleOptions{.defaultValue = a.fillAngle});
                if (!fill.ok())
                    co_return;
                if (std::abs(fill.value) < 1e-12) {
                    ctx.message("The angle to fill must not be zero.");
                    continue;
                }
                a.fillAngle = fill.value;
            } else if (k == "ROTate" || k == "Align") {
                cmd::KeywordOptions yo;
                yo.keywords = {"Yes", "No"};
                yo.defaultKeyword = yesNo(a.rotateItems);
                auto rot = co_await ctx.getKeyword(
                    std::string(k == "ROTate" ? "Rotate items as they are arrayed"
                                              : "Align items to the path") +
                        " [Yes/No] <" + *yo.defaultKeyword + ">",
                    yo);
                if (!rot.ok())
                    co_return;
                a.rotateItems = rot.keyword == "Yes";
            } else if (k == "Method") {
                cmd::KeywordOptions mo;
                mo.keywords = {"Divide", "Measure"};
                mo.defaultKeyword = a.pathSpacing > 0 ? "Measure" : "Divide";
                auto m = co_await ctx.getKeyword(
                    "Enter path method [Divide/Measure] <" + *mo.defaultKeyword + ">", mo);
                if (!m.ok())
                    co_return;
                if (m.keyword == "Divide") {
                    a.pathSpacing = 0;
                } else if (a.pathSpacing <= 0) {
                    // Measure keeps the items where Divide put them: the spacing they had
                    PolylineData pl;
                    pl.v = a.path;
                    pl.closed = a.pathClosed;
                    double total = 0;
                    for (const auto& seg : geom::segments(pl))
                        total += seg.curve.length();
                    const int gaps = a.pathClosed ? a.count : a.count - 1;
                    a.pathSpacing = gaps > 0 && total > 0 ? total / gaps : 1;
                }
            }
            geom::carryArrayOverrides(before, a);
            apply();
        }
        ctx.message("Array now has " + std::to_string(blocks::arrayItemCount(a)) + " items");
        ctx.setUndoLabel("Edit array");
        co_return;
    }
};

}  // namespace

std::unique_ptr<cmd::Command> makeArrayCommand() {
    return std::make_unique<ArrayCommand>();
}
std::unique_ptr<cmd::Command> makeArrayRectCommand() {
    return std::make_unique<ArrayCommand>(ArrayCommand::Mode::Rect);
}
std::unique_ptr<cmd::Command> makeArrayPolarCommand() {
    return std::make_unique<ArrayCommand>(ArrayCommand::Mode::Polar);
}
std::unique_ptr<cmd::Command> makeArrayPathCommand() {
    return std::make_unique<ArrayCommand>(ArrayCommand::Mode::Path);
}
std::unique_ptr<cmd::Command> makeArrayEditCommand() {
    return std::make_unique<ArrayEditCommand>();
}
std::unique_ptr<cmd::Command> makeArrayRemoveCommand() {
    return std::make_unique<ArrayRemoveCommand>();
}

namespace detail {

std::optional<std::pair<EntityId, int>> arrayItemNear(const Document& doc, Vec2 p, double tol) {
    std::optional<std::pair<EntityId, int>> hit;
    doc.forEachEntity([&](const Entity& e) {
        if (hit)
            return;
        const auto* ins = entityAs<InsertData>(e);
        if (!ins || !ins->array || doc.ownerBlock(e.common.id) || !editable(doc, e))
            return;
        if (!blocks::insertBounds(doc, e).inflated(tol).contains(p))
            return;
        if (auto item = blocks::arrayItemAt(doc, *ins, p, tol))
            hit = std::pair{e.common.id, *item};
    });
    return hit;
}

}  // namespace detail

}  // namespace tbcad::block_cmds
