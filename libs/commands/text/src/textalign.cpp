// TEXTALIGN: lines up several texts with the first one by their left, centre, right, top, middle
// or bottom edges.
#include "commands.h"
#include "tbcad/geom/entity.h"

namespace tbcad::textcmd {

namespace {

class TextAlignCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "TEXTALIGN"; }
    std::string_view description() const override {
        return "Line up text objects with the first one by an edge or a centre line";
    }

    cmd::Task run(cmd::CommandContext& ctx) override {
        cmd::SelectionOptions so;
        so.filter = [](const Entity& e) {
            return entityAs<TextData>(e) != nullptr || entityAs<MTextData>(e) != nullptr;
        };
        auto sel = co_await ctx.getSelection("Select text to align (the first stays)", so);
        if (!sel.ok())
            co_return;
        std::vector<EntityId> ids;
        for (EntityId id : sel.ids)
            if (const Entity* e = ctx.doc().find(id); e && so.filter(*e))
                ids.push_back(id);
        if (ids.size() < 2) {
            ctx.message("Select at least two text objects.");
            co_return;
        }
        cmd::KeywordOptions ko;
        ko.keywords = {"Left", "Center", "Right", "Top", "Middle", "Bottom"};
        ko.defaultKeyword = "Left";
        auto how =
            co_await ctx.getKeyword("Align by [Left/Center/Right/Top/Middle/Bottom] <Left>", ko);
        if (!how.ok())
            co_return;
        const BBox2 ref = geom::bounds(*ctx.doc().find(ids.front()), ctx.doc());
        ctx.setUndoLabel("TEXTALIGN");
        std::size_t n = 0;
        for (std::size_t i = 1; i < ids.size(); ++i) {
            const BBox2 b = geom::bounds(*ctx.doc().find(ids[i]), ctx.doc());
            Vec2 d{};
            if (how.keyword == "Left")
                d.x = ref.min.x - b.min.x;
            else if (how.keyword == "Right")
                d.x = ref.max.x - b.max.x;
            else if (how.keyword == "Center")
                d.x = ref.center().x - b.center().x;
            else if (how.keyword == "Bottom")
                d.y = ref.min.y - b.min.y;
            else if (how.keyword == "Top")
                d.y = ref.max.y - b.max.y;
            else
                d.y = ref.center().y - b.center().y;
            if (!ctx.tx().modify(ids[i], [d](Entity& e) {
                    if (auto* t = entityAs<TextData>(e)) {
                        t->position += d;
                        t->alignPoint += d;
                    } else if (auto* m = entityAs<MTextData>(e)) {
                        m->position += d;
                    }
                }))
                continue;
            ++n;
        }
        ctx.message(std::to_string(n) + " text objects aligned.");
    }
};

}  // namespace

std::unique_ptr<cmd::Command> makeTextAlignCommand() {
    return std::make_unique<TextAlignCommand>();
}

}  // namespace tbcad::textcmd
