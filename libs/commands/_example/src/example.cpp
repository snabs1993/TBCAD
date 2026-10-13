#include "tbcad/example/example.h"

namespace tbcad::example {

namespace {

// EXAMPLELINE: asks for two points and draws a line. The smallest useful command; copy it.
class ExampleLine final : public cmd::Command {
public:
    std::string_view name() const override { return "EXAMPLELINE"; }
    std::vector<std::string> defaultAliases() const override { return {"EXL"}; }
    std::string_view description() const override { return "Example: draw one line"; }

    cmd::Task run(cmd::CommandContext& ctx) override {
        auto first = co_await ctx.getPoint("Specify first point");
        if (!first.ok())
            co_return;  // Esc or Enter: nothing was changed

        cmd::PointOptions next;
        next.base = first.point;
        next.preview = [from = first.point](Vec2 cursor, DisplayList& out) {
            DisplayPolyline pl;
            pl.pts = {from, cursor};
            out.items.emplace_back(std::move(pl));
        };
        auto second = co_await ctx.getPoint("Specify second point", std::move(next));
        if (!second.ok())
            co_return;  // Esc: the transaction is rolled back, the drawing stays untouched

        auto id = ctx.tx().add(Entity{{}, LineData{first.point, second.point}});
        if (!id)
            ctx.message(id.error().message);
    }
};

}  // namespace

void registerCommands(cmd::Registry& registry) {
    (void)registry.add(std::make_unique<ExampleLine>());
}

}  // namespace tbcad::example
