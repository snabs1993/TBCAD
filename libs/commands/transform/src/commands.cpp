#include <cmath>

#include "tbcad/geom/geom.h"
#include "tbcad/transform/apply.h"
#include "tbcad/transform/grips.h"
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
        auto base = co_await ctx.getPoint("Specify base point");
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
        auto base = co_await ctx.getPoint("Specify base point");
        if (!base.ok())
            co_return;
        bool first = true;
        for (;;) {
            PointOptions o;
            o.base = base.point;
            o.keywords = {"Exit"};
            o.preview = previewOf(ctx, ids, [b = base.point](Vec2 c) -> std::optional<Transform2> {
                return Transform2::translate(c - b);
            });
            auto to = co_await ctx.getPoint(
                first ? "Specify second point or <use first point as displacement>"
                      : "Specify second point or [Exit]",
                std::move(o));
            if (to.status == Status::None && first) {
                report(ctx, applyTransform(ctx, ids, Transform2::translate(base.point), true));
                co_return;
            }
            if (!to.ok())
                co_return;  // Enter, Exit or Esc ends the repeat
            first = false;
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

}  // namespace

void registerCommands(cmd::Registry& registry) {
    (void)registry.add(std::make_unique<MoveCommand>());
    (void)registry.add(std::make_unique<CopyCommand>());
    (void)registry.add(std::make_unique<RotateCommand>());
    (void)registry.add(std::make_unique<ScaleCommand>());
    (void)registry.add(std::make_unique<MirrorCommand>());
    registerGripCommand(registry);
}

}  // namespace tbcad::transform
