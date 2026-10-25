#include "commands.h"
#include "common.h"

namespace tbcad::draw2::detail {

namespace {

// POINT [PO]: places point objects until Esc or Enter; the ones already placed stay.
class PointCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "POINT"; }
    std::vector<std::string> defaultAliases() const override { return {"PO"}; }
    std::string_view description() const override { return "Place point objects"; }

    cmd::Task run(cmd::CommandContext& ctx) override {
        ctx.commitOnCancel();
        for (;;) {
            auto r = co_await ctx.getPoint("Specify a point");
            if (!r.ok())
                co_return;
            ctx.checkpoint();
            addEntity(ctx, PointData{r.point});
        }
    }
};

// PTYPE: the point display style, stored in the drawing (Header::custom "pdmode" / "pdsize") like
// the PDMODE and PDSIZE variables. pdmode: 0 dot, 1 nothing, 2 plus, 3 cross, 4 tick, plus 32 for
// a circle and 64 for a square around the mark. pdsize: size in mm (0 = 5% of the screen).
class PointTypeCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "PTYPE"; }
    std::vector<std::string> defaultAliases() const override { return {"PDMODE"}; }
    std::string_view description() const override { return "Set the point display style and size"; }

    cmd::Task run(cmd::CommandContext& ctx) override {
        const auto& custom = ctx.doc().header().custom;
        long long curMode = 0;
        double curSize = 0;
        if (auto it = custom.find("pdmode"); it != custom.end()) {
            try {
                curMode = std::stoll(it->second);
            } catch (...) {
            }
        }
        if (auto it = custom.find("pdsize"); it != custom.end()) {
            try {
                curSize = std::stod(it->second);
            } catch (...) {
            }
        }

        cmd::IntOptions mo;
        mo.defaultValue = curMode;
        mo.min = 0;
        mo.max = 4 + 32 + 64;
        auto mode = co_await ctx.getInteger(
            "Point style (0 dot, 1 none, 2 plus, 3 cross, 4 tick; +32 circle, +64 square) <" +
                std::to_string(curMode) + ">",
            std::move(mo));
        if (!mode.ok())
            co_return;
        const long long base = mode.value % 32;
        if (base > 4) {
            ctx.message("The point style must be 0 to 4, plus 32 and/or 64.");
            co_return;
        }
        cmd::DistanceOptions so;
        so.defaultValue = curSize;
        so.allowNegative = false;
        auto size =
            co_await ctx.getDistance("Point size in mm (0 = screen relative)", std::move(so));
        if (!size.ok())
            co_return;
        auto done = ctx.tx().modifyHeader([&](Header& h) {
            h.custom["pdmode"] = std::to_string(mode.value);
            char buf[64];
            std::snprintf(buf, sizeof buf, "%.17g", size.value);
            h.custom["pdsize"] = buf;
        });
        if (!done)
            ctx.message(done.error().message);
    }
};

// DDPTYPE: opens the Point Style dialog; the dialog applies the choice through PTYPE.
class PointStyleDialogCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "DDPTYPE"; }
    std::string_view description() const override { return "Open the Point Style dialog"; }
    cmd::Task run(cmd::CommandContext& ctx) override {
        ctx.host().openPanel("pointstyle");
        co_return;
    }
};

}  // namespace

std::unique_ptr<cmd::Command> makePointStyleDialog() {
    return std::make_unique<PointStyleDialogCommand>();
}
std::unique_ptr<cmd::Command> makePoint() {
    return std::make_unique<PointCommand>();
}
std::unique_ptr<cmd::Command> makePointType() {
    return std::make_unique<PointTypeCommand>();
}

}  // namespace tbcad::draw2::detail
