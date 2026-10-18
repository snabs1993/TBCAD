#include <cmath>

#include "commands.h"
#include "common.h"

namespace tbcad::draw2::detail {

namespace {

constexpr const char* kSize = "breakline.size";

// A line from a to b with the break symbol (a Z shaped zigzag) at `mid`. `size` is the width of
// the symbol along the line; the zigzag is as high as it is wide.
std::optional<PolylineData> breakLine(Vec2 a, Vec2 b, Vec2 mid, double size) {
    const Vec2 d = b - a;
    const double len = std::sqrt(d.x * d.x + d.y * d.y);
    if (len < 1e-9 || !(size > 0))
        return std::nullopt;
    const Vec2 u = d * (1 / len);
    const Vec2 n{-u.y, u.x};
    double t = (mid.x - a.x) * u.x + (mid.y - a.y) * u.y;  // where the symbol sits along the line
    t = std::clamp(t, size / 2, std::max(size / 2, len - size / 2));
    const Vec2 c = a + u * t;
    PolylineData p;
    for (const Vec2 q : {a, c - u * (size / 2), c - u * (size / 4) + n * (size / 2),
                         c + u * (size / 4) - n * (size / 2), c + u * (size / 2), b}) {
        PolyVertex v;
        v.p = q;
        p.v.push_back(v);
    }
    return p;
}

// BREAKLINE [BRL]: a line with a break symbol; picks the ends, then where the symbol goes.
class BreakLineCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "BREAKLINE"; }
    std::vector<std::string> defaultAliases() const override { return {"BRL"}; }
    std::string_view description() const override { return "Draw a line with a break symbol"; }

    cmd::Task run(cmd::CommandContext& ctx) override {
        auto* c = &ctx;
        ctx.commitOnCancel();
        double size = settingNumber(ctx, kSize, 10);
        Vec2 a{}, b{};
        for (;;) {
            cmd::PointOptions po;
            po.keywords = {"Size"};
            auto r = co_await ctx.getPoint("Specify first point of the break line or [Size]", po);
            if (r.status == cmd::Status::Keyword) {
                cmd::DistanceOptions so;
                so.defaultValue = size;
                so.allowZero = false;
                auto s = co_await ctx.getDistance("Width of the break symbol", std::move(so));
                if (!s.ok())
                    co_return;
                size = s.value;
                saveSetting(ctx, kSize, size);
                continue;
            }
            if (!r.ok())
                co_return;
            a = r.point;
            break;
        }
        {
            cmd::PointOptions po;
            po.base = a;
            auto r = co_await ctx.getPoint("Specify second point of the break line", po);
            if (!r.ok())
                co_return;
            b = r.point;
        }
        cmd::PointOptions po;
        po.base = a;
        const Vec2 mid0 = (a + b) * 0.5;
        po.preview = [c, a, b, size](Vec2 cursor, DisplayList& out) {
            if (auto p = breakLine(a, b, cursor, size))
                previewEntity(*c, makeEntity(std::move(*p)), out);
        };
        auto m =
            co_await ctx.getPoint("Specify the place of the break symbol <middle>", std::move(po));
        Vec2 mid = mid0;
        if (m.ok())
            mid = m.point;
        else if (m.status != cmd::Status::None)
            co_return;
        ctx.checkpoint();
        if (auto p = breakLine(a, b, mid, size))
            addEntity(ctx, std::move(*p));
        else
            ctx.message("The two points are the same.");
    }
};

}  // namespace

std::unique_ptr<cmd::Command> makeBreakLine() {
    return std::make_unique<BreakLineCommand>();
}

}  // namespace tbcad::draw2::detail
