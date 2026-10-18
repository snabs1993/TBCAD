#include <cmath>

#include "tbcad/geom/geom.h"
#include "tbcad/transform/apply.h"
#include "tbcad/transform/grips.h"
#include "tbcad/transform/stretch.h"
#include "tbcad/transform/transform.h"

namespace tbcad::transform {

namespace {

using cmd::AngleOptions;
using cmd::CommandContext;
using cmd::DistanceOptions;
using cmd::KeywordOptions;
using cmd::PointOptions;
using cmd::SelectionOptions;
using cmd::Status;
using cmd::Task;

constexpr double kEps = 1e-12;

// Asks for the objects (pre-selection first); `out` stays empty when the user gave up.
Task askObjects(CommandContext& ctx, std::vector<EntityId>& out) {
    auto sel = co_await ctx.getSelection("Select objects");
    if (sel.ok())
        out = std::move(sel.ids);
}

void report(CommandContext& ctx, const ApplyReport& rep) {
    if (rep.failed > 0)
        ctx.message(std::to_string(rep.failed) +
                    " object(s) could not be transformed: " + rep.firstError);
}

// Rubber-band preview of the selection under a transform that depends on the cursor.
template <class MakeTransform>
cmd::PreviewFn previewOf(CommandContext& ctx, const std::vector<EntityId>& ids,
                         MakeTransform make) {
    return [&ctx, &ids, make](Vec2 cursor, DisplayList& out) {
        auto t = make(cursor);
        if (t)
            buildPreview(ctx.doc(), ids, *t, ctx.host().view().scale, out);
    };
}

// COPY > Array: a row of copies. A second point sets the spacing between neighbours, Fit makes
// it the position of the last copy.
Task arrayCopies(CommandContext& ctx, const std::vector<EntityId>& ids, Vec2 base) {
    cmd::IntOptions io;
    io.min = 2;
    io.max = 100000;
    io.defaultValue = 5;
    auto count = co_await ctx.getInteger("Enter the number of items to array <5>", io);
    if (!count.ok())
        co_return;
    const long long n = count.value;
    bool fit = false;
    for (;;) {
        PointOptions o;
        o.base = base;
        o.keywords = {"Fit"};
        o.allowNone = false;
        o.preview = [&ctx, &ids, base, n, &fit](Vec2 c, DisplayList& out) {
            const Vec2 step = fit ? (c - base) * (1.0 / static_cast<double>(n - 1)) : c - base;
            const double px = ctx.host().view().scale;
            if (n > 50) {  // keep the preview light: first and last copy only
                buildPreview(ctx.doc(), ids, Transform2::translate(step), px, out);
                buildPreview(ctx.doc(), ids, Transform2::translate(step * double(n - 1)), px, out);
                return;
            }
            for (long long k = 1; k < n; ++k)
                buildPreview(ctx.doc(), ids, Transform2::translate(step * double(k)), px, out);
        };
        auto to = co_await ctx.getPoint(
            fit ? "Specify second point to fit the array between" : "Specify second point or [Fit]",
            std::move(o));
        if (to.status == Status::Keyword && to.keyword == "Fit") {
            fit = true;
            continue;
        }
        if (!to.ok())
            co_return;
        const Vec2 step =
            fit ? (to.point - base) * (1.0 / static_cast<double>(n - 1)) : to.point - base;
        for (long long k = 1; k < n; ++k)
            report(ctx, applyTransform(ctx, ids, Transform2::translate(step * double(k)), true));
        co_return;
    }
}

class MoveCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "MOVE"; }
    std::vector<std::string> defaultAliases() const override { return {"M"}; }
    std::string_view description() const override { return "Move objects"; }

    Task run(CommandContext& ctx) override {
        std::vector<EntityId> ids;
        co_await askObjects(ctx, ids);
        if (ids.empty())
            co_return;
        PointOptions bo;
        bo.keywords = {"Displacement"};
        auto base = co_await ctx.getPoint("Specify base point or [Displacement]", std::move(bo));
        if (base.status == Status::Keyword) {
            auto d = co_await ctx.getPoint("Specify displacement");
            if (d.ok())
                report(ctx, applyTransform(ctx, ids, Transform2::translate(d.point), false));
            co_return;
        }
        if (!base.ok())
            co_return;
        PointOptions o;
        o.base = base.point;
        o.preview = previewOf(ctx, ids, [b = base.point](Vec2 c) -> std::optional<Transform2> {
            return Transform2::translate(c - b);
        });
        auto to = co_await ctx.getPoint("Specify second point or <use first point as displacement>",
                                        std::move(o));
        Vec2 delta;
        if (to.status == Status::None)
            delta = base.point;  // Enter: the base point is the displacement itself
        else if (to.ok())
            delta = to.point - base.point;
        else
            co_return;
        report(ctx, applyTransform(ctx, ids, Transform2::translate(delta), false));
    }
};

class CopyCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "COPY"; }
    std::vector<std::string> defaultAliases() const override { return {"CO", "CP"}; }
    std::string_view description() const override { return "Copy objects (repeats until Enter)"; }

    Task run(CommandContext& ctx) override {
        ctx.commitOnCancel();  // Esc after some copies keeps them
        std::vector<EntityId> ids;
        co_await askObjects(ctx, ids);
        if (ids.empty())
            co_return;
        PointOptions bo;
        bo.keywords = {"Displacement"};
        auto base = co_await ctx.getPoint("Specify base point or [Displacement]", std::move(bo));
        if (base.status == Status::Keyword) {
            auto d = co_await ctx.getPoint("Specify displacement");
            if (d.ok())
                report(ctx, applyTransform(ctx, ids, Transform2::translate(d.point), true));
            co_return;
        }
        if (!base.ok())
            co_return;
        bool first = true;
        int copies = 0;
        for (;;) {
            PointOptions o;
            o.base = base.point;
            o.keywords = {"Array", "Exit"};
            o.preview = previewOf(ctx, ids, [b = base.point](Vec2 c) -> std::optional<Transform2> {
                return Transform2::translate(c - b);
            });
            auto to = co_await ctx.getPoint(
                first ? "Specify second point or [Array] <use first point as displacement>"
                      : "Specify second point or [Array/Exit] (" + std::to_string(copies) +
                            (copies == 1 ? " copy" : " copies") + " made, Enter to finish)",
                std::move(o));
            if (to.status == Status::Keyword && to.keyword == "Array") {
                co_await arrayCopies(ctx, ids, base.point);
                co_return;
            }
            if (to.status == Status::None && first) {
                report(ctx, applyTransform(ctx, ids, Transform2::translate(base.point), true));
                co_return;
            }
            if (!to.ok())
                co_return;  // Enter, Exit or Esc ends the repeat
            first = false;
            ++copies;
            report(ctx,
                   applyTransform(ctx, ids, Transform2::translate(to.point - base.point), true));
        }
    }
};

class RotateCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "ROTATE"; }
    std::vector<std::string> defaultAliases() const override { return {"RO"}; }
    std::string_view description() const override { return "Rotate objects about a base point"; }

    Task run(CommandContext& ctx) override {
        std::vector<EntityId> ids;
        co_await askObjects(ctx, ids);
        if (ids.empty())
            co_return;
        auto base = co_await ctx.getPoint("Specify base point");
        if (!base.ok())
            co_return;
        const Vec2 b = base.point;
        bool copy = false;
        double angle = 0;
        for (;;) {
            AngleOptions o;
            o.base = b;
            o.keywords = {"Copy", "Reference"};
            o.preview = previewOf(ctx, ids, [b](Vec2 c) -> std::optional<Transform2> {
                return Transform2::rotate(std::atan2(c.y - b.y, c.x - b.x), b);
            });
            auto r = co_await ctx.getAngle(copy ? "Specify rotation angle or [Reference] (copy)"
                                                : "Specify rotation angle or [Copy/Reference]",
                                           std::move(o));
            if (r.status == Status::Keyword && r.keyword == "Copy") {
                copy = true;
                ctx.message("Rotating a copy of the selected objects.");
                continue;
            }
            if (r.status == Status::Keyword && r.keyword == "Reference") {
                AngleOptions ro;
                ro.base = b;
                ro.defaultValue = 0.0;
                auto ref = co_await ctx.getAngle("Specify the reference angle <0>", std::move(ro));
                if (!ref.ok())
                    co_return;
                const double refAngle = ref.value;
                AngleOptions no;
                no.base = b;
                no.preview =
                    previewOf(ctx, ids, [b, refAngle](Vec2 c) -> std::optional<Transform2> {
                        return Transform2::rotate(std::atan2(c.y - b.y, c.x - b.x) - refAngle, b);
                    });
                auto next = co_await ctx.getAngle("Specify the new angle", std::move(no));
                if (!next.ok())
                    co_return;
                angle = next.value - refAngle;
                break;
            }
            if (!r.ok())
                co_return;
            angle = r.value;
            break;
        }
        report(ctx, applyTransform(ctx, ids, Transform2::rotate(angle, b), copy));
    }
};

class ScaleCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "SCALE"; }
    std::vector<std::string> defaultAliases() const override { return {"SC"}; }
    std::string_view description() const override { return "Scale objects uniformly"; }

    Task run(CommandContext& ctx) override {
        std::vector<EntityId> ids;
        co_await askObjects(ctx, ids);
        if (ids.empty())
            co_return;
        auto base = co_await ctx.getPoint("Specify base point");
        if (!base.ok())
            co_return;
        const Vec2 b = base.point;
        bool copy = false;
        double factor = 1;
        for (;;) {
            DistanceOptions o;
            o.base = b;
            o.keywords = {"Copy", "Reference"};
            o.allowZero = false;
            o.preview = previewOf(ctx, ids, [b](Vec2 c) -> std::optional<Transform2> {
                const double f = distance(c, b);
                if (f < kEps)
                    return std::nullopt;
                return Transform2::scale(f, f, b);
            });
            auto r = co_await ctx.getDistance(copy ? "Specify scale factor or [Reference] (copy)"
                                                   : "Specify scale factor or [Copy/Reference]",
                                              std::move(o));
            if (r.status == Status::Keyword && r.keyword == "Copy") {
                copy = true;
                ctx.message("Scaling a copy of the selected objects.");
                continue;
            }
            if (r.status == Status::Keyword && r.keyword == "Reference") {
                DistanceOptions ro;
                ro.base = b;
                ro.defaultValue = 1.0;
                ro.allowZero = false;
                auto ref = co_await ctx.getDistance("Specify reference length <1>", std::move(ro));
                if (!ref.ok())
                    co_return;
                const double refLen = ref.value;
                DistanceOptions no;
                no.base = b;
                no.allowZero = false;
                no.preview = previewOf(ctx, ids, [b, refLen](Vec2 c) -> std::optional<Transform2> {
                    const double f = distance(c, b) / refLen;
                    if (f < kEps)
                        return std::nullopt;
                    return Transform2::scale(f, f, b);
                });
                auto next = co_await ctx.getDistance("Specify new length", std::move(no));
                if (!next.ok())
                    co_return;
                factor = next.value / refLen;
                break;
            }
            if (!r.ok())
                co_return;
            factor = r.value;
            break;
        }
        if (!(factor > kEps)) {
            ctx.message("The scale factor must be greater than zero.");
            co_return;
        }
        report(ctx, applyTransform(ctx, ids, Transform2::scale(factor, factor, b), copy));
    }
};

class MirrorCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "MIRROR"; }
    std::vector<std::string> defaultAliases() const override { return {"MI"}; }
    std::string_view description() const override {
        return "Mirror objects about a line (text stays readable)";
    }

    Task run(CommandContext& ctx) override {
        std::vector<EntityId> ids;
        co_await askObjects(ctx, ids);
        if (ids.empty())
            co_return;
        auto a = co_await ctx.getPoint("Specify first point of mirror line");
        if (!a.ok())
            co_return;
        Vec2 b;
        for (;;) {
            PointOptions o;
            o.base = a.point;
            o.preview = previewOf(ctx, ids, [p = a.point](Vec2 c) -> std::optional<Transform2> {
                auto m = Transform2::mirror(p, c);
                if (!m)
                    return std::nullopt;
                return m.value();
            });
            auto r = co_await ctx.getPoint("Specify second point of mirror line", std::move(o));
            if (!r.ok())
                co_return;
            if (distance(r.point, a.point) < kEps) {
                ctx.message("The two points of the mirror line must differ.");
                continue;
            }
            b = r.point;
            break;
        }
        auto m = Transform2::mirror(a.point, b);
        if (!m) {
            ctx.message(m.error().message);
            co_return;
        }
        KeywordOptions ko;
        ko.keywords = {"Yes", "No"};
        ko.defaultKeyword = "No";
        auto erase = co_await ctx.getKeyword("Erase source objects? [Yes/No] <No>", std::move(ko));
        if (!erase.ok())
            co_return;
        report(ctx, applyTransform(ctx, ids, m.value(), erase.keyword != "Yes"));
    }
};

// Entities a crossing window can stretch: visible, unlocked, not inside a block definition.
std::vector<EntityId> stretchCandidates(CommandContext& ctx, const BBox2& window) {
    const Document& doc = ctx.doc();
    std::vector<EntityId> out;
    auto consider = [&](const Entity& e, bool test) {
        if (!e.common.visible || doc.ownerBlock(e.common.id))
            return;
        const EffectiveLayer layer = doc.effective(e.common);
        if (!layer.visible() || layer.locked)
            return;
        if (test && !window.intersects(geom::bounds(e, doc)))
            return;
        out.push_back(e.common.id);
    };
    if (auto ids = ctx.host().entitiesInWindow(window)) {
        for (EntityId id : *ids)
            if (const Entity* e = doc.find(id))
                consider(*e, true);
    } else {
        doc.forEachEntity([&](const Entity& e) { consider(e, true); });
    }
    return out;
}

class StretchCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "STRETCH"; }
    std::vector<std::string> defaultAliases() const override { return {"S"}; }
    std::string_view description() const override {
        return "Stretch the vertices inside a crossing window";
    }

    Task run(CommandContext& ctx) override {
        auto c1 = co_await ctx.getPoint("Specify first corner of the crossing window");
        if (!c1.ok())
            co_return;
        PointOptions co;
        co.base = c1.point;
        co.preview = [first = c1.point](Vec2 c, DisplayList& out) {  // the crossing window
            DisplayPolyline pl;
            pl.closed = true;
            pl.pts = {first, {c.x, first.y}, c, {first.x, c.y}};
            out.items.emplace_back(std::move(pl));
        };
        auto c2 = co_await ctx.getPoint("Specify opposite corner", std::move(co));
        if (!c2.ok())
            co_return;
        BBox2 window;
        window.expand(c1.point);
        window.expand(c2.point);
        const std::vector<EntityId> ids = stretchCandidates(ctx, window);
        if (ids.empty()) {
            ctx.message("Nothing to stretch in the window.");
            co_return;
        }
        // Only objects with an end point or vertex inside the window move; say how many do.
        const std::size_t movable = stretchByWindow(ctx.doc(), ids, window, {1, 0}).size();
        if (movable == 0) {
            ctx.message(
                "No end point or vertex lies inside the crossing window; nothing to stretch.");
            co_return;
        }
        ctx.message(std::to_string(movable) + " found in the crossing window.");
        auto base = co_await ctx.getPoint("Specify base point");
        if (!base.ok())
            co_return;
        PointOptions o;
        o.base = base.point;
        o.preview = [&ctx, &ids, window, b = base.point](Vec2 c, DisplayList& out) {
            geom::DisplayContext dc;
            dc.pixelSizeMm = ctx.host().view().scale > 0 ? ctx.host().view().scale : 1;
            for (auto& [id, e] : stretchByWindow(ctx.doc(), ids, window, c - b))
                geom::buildDisplay(ctx.doc(), e, dc, out);
        };
        auto to = co_await ctx.getPoint("Specify second point or <use first point as displacement>",
                                        std::move(o));
        Vec2 delta;
        if (to.status == Status::None)
            delta = base.point;
        else if (to.ok())
            delta = to.point - base.point;
        else
            co_return;
        std::string err;
        std::size_t n = 0;
        for (auto& [id, e] : stretchByWindow(ctx.doc(), ids, window, delta, &err)) {
            Entity moved = std::move(e);
            if (ctx.tx().modify(id, [&](Entity& tgt) { tgt.data = std::move(moved.data); }))
                ++n;
        }
        if (!err.empty())
            ctx.message("Some objects could not be stretched: " + err);
        ctx.message(std::to_string(n) + " object(s) stretched.");
    }
};

}  // namespace

void registerCommands(cmd::Registry& registry) {
    (void)registry.add(std::make_unique<MoveCommand>());
    (void)registry.add(std::make_unique<CopyCommand>());
    (void)registry.add(std::make_unique<RotateCommand>());
    (void)registry.add(std::make_unique<ScaleCommand>());
    (void)registry.add(std::make_unique<MirrorCommand>());
    (void)registry.add(std::make_unique<StretchCommand>());
    registerGripCommand(registry);
}

}  // namespace tbcad::transform
