// ADDSELECTED: make the layer and properties of a picked object the ones new objects get.
#include "commands.h"
#include "tbcad/layers/layer_ops.h"

namespace tbcad::sysvars {

namespace {

class AddSelectedCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "ADDSELECTED"; }
    std::string_view description() const override {
        return "New objects get the layer and properties of a picked object";
    }
    cmd::Task run(cmd::CommandContext& ctx) override {
        auto pick = co_await ctx.getEntity("Select an object whose properties new objects take",
                                           cmd::EntityOptions{});
        if (!pick.ok())
            co_return;
        const Entity* e = ctx.doc().find(pick.id);
        if (!e)
            co_return;
        const EntityCommon c = e->common;
        ctx.setUndoLabel("ADDSELECTED");
        auto r = ctx.tx().modifyHeader([&](Header& h) {
            if (c.layer)
                h.currentLayer = c.layer;
            if (c.level)
                h.currentLevel = c.level;
            h.currentColor = c.color.kind == ColorKind::ByParent ? Color::byLayer() : c.color;
            h.currentLinetype = c.linetype;
            h.currentLineweight = c.lineweight.isByParent() ? LineWeight::byLayer() : c.lineweight;
            h.currentTransparency = c.transparency;
        });
        if (!r) {
            ctx.message(r.error().message);
            co_return;
        }
        const Layer* l = ctx.doc().layer(ctx.doc().header().currentLayer);
        ctx.message("New objects now use layer " + (l ? l->name : std::string("0")) + ", colour " +
                    layers::colorName(c.color) + ", linetype " +
                    layers::linetypeName(ctx.doc(), c.linetype) + ". Draw the next object.");
    }
};

}  // namespace

std::unique_ptr<cmd::Command> makeAddSelectedCommand() {
    return std::make_unique<AddSelectedCommand>();
}

}  // namespace tbcad::sysvars
