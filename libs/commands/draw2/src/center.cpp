#include <algorithm>

#include "commands.h"
#include "common.h"

namespace tbcad::draw2::detail {

namespace {

constexpr const char* kExtension = "center.extension";
constexpr double kDefaultExtension = 2.5;

bool isCircleOrArc(const Entity& e) {
    return kind(e) == EntityKind::Circle || kind(e) == EntityKind::Arc;
}
bool isLine(const Entity& e) {
    return kind(e) == EntityKind::Line;
}

// Asks for the extension when the user chose that keyword.
cmd::Task askExtension(cmd::CommandContext& ctx) {
    cmd::DistanceOptions o;
    o.defaultValue = settingNumber(ctx, kExtension, kDefaultExtension);
    o.allowNegative = false;
    auto r = co_await ctx.getDistance("Specify extension past the object", std::move(o));
    if (r.ok())
        saveSetting(ctx, kExtension, r.value);
}

// CENTERMARK [CM]: a cross through the centre of a circle or arc that reaches past it by the
// extension. Plain lines; picks repeat until Enter or Esc.
class CenterMarkCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "CENTERMARK"; }
    std::vector<std::string> defaultAliases() const override { return {"CM", "DIMCENTER"}; }
    std::string_view description() const override { return "Mark the centre of a circle or arc"; }

    cmd::Task run(cmd::CommandContext& ctx) override {
        ctx.commitOnCancel();
        for (;;) {
            cmd::EntityOptions eo;
            eo.filter = isCircleOrArc;
            eo.keywords = {"Extension"};
            auto r = co_await ctx.getEntity(promptWith("Select circle or arc to mark", eo.keywords),
                                            std::move(eo));
            if (r.status == cmd::Status::Keyword) {
                co_await askExtension(ctx);
                continue;
            }
            if (!r.ok())
                co_return;
            const Entity* e = ctx.doc().find(r.id);
            if (!e)
                continue;
            Vec2 c{};
            double radius = 0;
            if (const auto* ci = std::get_if<CircleData>(&e->data)) {
                c = ci->center;
                radius = ci->radius;
            } else if (const auto* a = std::get_if<ArcData>(&e->data)) {
                c = a->center;
                radius = a->radius;
            }
            const double h = radius + settingNumber(ctx, kExtension, kDefaultExtension);
            ctx.checkpoint();
            addEntity(ctx, LineData{{c.x - h, c.y}, {c.x + h, c.y}});
            addEntity(ctx, LineData{{c.x, c.y - h}, {c.x, c.y + h}});
        }
    }
};

// CENTERLINE [CL]: the line midway between two parallel lines, over the part where they overlap,
// reaching past each end by the extension.
class CenterLineCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "CENTERLINE"; }
    std::vector<std::string> defaultAliases() const override { return {"CL"}; }
    std::string_view description() const override {
        return "Draw the centreline between two lines";
    }

    cmd::Task run(cmd::CommandContext& ctx) override {
        for (;;) {
            cmd::EntityOptions eo;
            eo.filter = isLine;
            eo.keywords = {"Extension"};
            auto first = co_await ctx.getEntity(promptWith("Select first line", eo.keywords), eo);
            if (first.status == cmd::Status::Keyword) {
                co_await askExtension(ctx);
                continue;
            }
            if (!first.ok())
                co_return;
            cmd::EntityOptions so;
            so.filter = isLine;
            auto second = co_await ctx.getEntity("Select second line", std::move(so));
            if (!second.ok())
                co_return;
            const Entity* e1 = ctx.doc().find(first.id);
            const Entity* e2 = ctx.doc().find(second.id);
            if (!e1 || !e2 || first.id == second.id) {
                ctx.message("Select two different lines.");
                co_return;
            }
            const auto& l1 = std::get<LineData>(e1->data);
            const auto& l2 = std::get<LineData>(e2->data);
            const Vec2 u = normalized(l1.b - l1.a);
            const Vec2 u2 = normalized(l2.b - l2.a);
            if (tol::isZeroLength(l1.b - l1.a) || std::abs(cross(u, u2)) > 1e-6) {
                ctx.message("The two lines must be parallel.");
                co_return;
            }
            const double offset = dot(l2.a - l1.a, perp(u));
            if (std::abs(offset) < tol::kLinear) {
                ctx.message("The two lines are on top of each other.");
                co_return;
            }
            auto interval = [&](const LineData& l) {
                const double a = dot(l.a - l1.a, u), b = dot(l.b - l1.a, u);
                return std::pair{std::min(a, b), std::max(a, b)};
            };
            const auto i1 = interval(l1), i2 = interval(l2);
            const double lo = std::max(i1.first, i2.first), hi = std::min(i1.second, i2.second);
            if (hi - lo < tol::kLinear) {
                ctx.message("The two lines do not overlap.");
                co_return;
            }
            const double ext = settingNumber(ctx, kExtension, kDefaultExtension);
            const Vec2 origin = l1.a + perp(u) * (offset / 2);
            addEntity(ctx, LineData{origin + u * (lo - ext), origin + u * (hi + ext)});
            co_return;
        }
    }
};

}  // namespace

std::unique_ptr<cmd::Command> makeCenterMark() {
    return std::make_unique<CenterMarkCommand>();
}
std::unique_ptr<cmd::Command> makeCenterLine() {
    return std::make_unique<CenterLineCommand>();
}

}  // namespace tbcad::draw2::detail
