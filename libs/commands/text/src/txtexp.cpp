// TXTEXP: explodes text into closed polylines that follow the outlines of its letters.
#include "commands.h"
#include "tbcad/geom/entity.h"
#include "tbcad/text/text.h"

namespace tbcad::textcmd {

namespace {

class TxtExpCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "TXTEXP"; }
    std::string_view description() const override {
        return "Explode text into polylines along the letter outlines";
    }

    cmd::Task run(cmd::CommandContext& ctx) override {
        cmd::SelectionOptions so;
        so.filter = [](const Entity& e) {
            const auto* t = entityAs<TextData>(e);
            return (t && !t->attdef) || entityAs<MTextData>(e) != nullptr;
        };
        auto sel = co_await ctx.getSelection("Select text to explode", so);
        if (!sel.ok())
            co_return;
        ctx.setUndoLabel("TXTEXP");
        std::size_t texts = 0, rings = 0;
        for (EntityId id : sel.ids) {
            const Entity* e = ctx.doc().find(id);
            if (!e)
                continue;
            const auto* t = entityAs<TextData>(*e);
            const double h = t ? t->height : entityAs<MTextData>(*e)->height;
            geom::DisplayContext dc;
            dc.pixelSizeMm = h / 100;  // smooth enough to cut or plot at any size
            dc.chordTolMm = h / 500;
            DisplayList out;
            geom::buildDisplay(ctx.doc(), *e, dc, out);
            std::vector<Entity> made;
            for (const DisplayItem& item : out.items) {
                const auto* fill = std::get_if<DisplayFill>(&item);
                if (!fill)
                    continue;
                for (const std::vector<Vec2>& ring : fill->rings) {
                    if (ring.size() < 3)
                        continue;
                    PolylineData pl;
                    pl.closed = true;
                    for (Vec2 p : ring)
                        pl.v.push_back({p, 0, 0, 0});
                    Entity n;
                    n.common = e->common;
                    n.common.annotative = false;
                    n.common.annoScales = {};
                    n.data = std::move(pl);
                    made.push_back(std::move(n));
                }
            }
            if (made.empty())
                continue;
            if (!ctx.tx().erase(id))
                continue;
            for (Entity& n : made) {
                if (auto added = ctx.tx().add(std::move(n)); !added) {
                    ctx.message(added.error().message);
                    co_return;
                }
                ++rings;
            }
            ++texts;
        }
        ctx.message(std::to_string(texts) + " text objects exploded into " + std::to_string(rings) +
                    " outlines.");
    }
};

}  // namespace

std::unique_ptr<cmd::Command> makeTxtExpCommand() {
    return std::make_unique<TxtExpCommand>();
}

}  // namespace tbcad::textcmd
