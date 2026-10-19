// TEXTALIGN: lines up several texts with the first one by their left, centre, right, top, middle
// or bottom edges, or as in AutoCAD along a line from the first text through a picked point:
// Points (each text slides onto the line), Distribute (spread evenly up to the point) and Spacing
// (a fixed distance apart along the line).
#include <algorithm>

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
        ko.keywords = {"Left",   "Center", "Right",      "Top",    "Middle",
                       "Bottom", "Points", "Distribute", "Spacing"};
        ko.defaultKeyword = "Left";
        auto how = co_await ctx.getKeyword(
            "Align by [Left/Center/Right/Top/Middle/Bottom] or along a line "
            "[Points/Distribute/Spacing] <Left>",
            ko);
        if (!how.ok())
            co_return;
        const BBox2 ref = geom::bounds(*ctx.doc().find(ids.front()), ctx.doc());
        if (how.keyword == "Points" || how.keyword == "Distribute" || how.keyword == "Spacing") {
            double spacing = 0;
            if (how.keyword == "Spacing") {
                cmd::DistanceOptions dopt;
                auto sp = co_await ctx.getDistance("Spacing between the texts", dopt);
                if (!sp.ok())
                    co_return;
                spacing = sp.value;
            }
            // The line runs from the first text's lower left corner through the picked point;
            // every text is moved by its own lower left corner.
            const Vec2 a = ref.min;
            cmd::PointOptions po;
            po.base = a;
            auto p2 = co_await ctx.getPoint("Pick second point", po);
            if (!p2.ok())
                co_return;
            const Vec2 ab = p2.point - a;
            const double len = length(ab);
            if (len < tol::kLinear) {
                ctx.message("The second point is on the first text.");
                co_return;
            }
            const Vec2 u = ab / len;
            struct Spot {
                EntityId id;
                Vec2 corner;
            };
            std::vector<Spot> spots;
            for (std::size_t i = 1; i < ids.size(); ++i)
                spots.push_back({ids[i], geom::bounds(*ctx.doc().find(ids[i]), ctx.doc()).min});
            std::stable_sort(spots.begin(), spots.end(), [&](const Spot& x, const Spot& y) {
                return dot(x.corner - a, u) < dot(y.corner - a, u);
            });
            ctx.setUndoLabel("TEXTALIGN");
            const double n = static_cast<double>(spots.size());
            for (std::size_t i = 0; i < spots.size(); ++i) {
                double along = dot(spots[i].corner - a, u);  // Points: keep the place along
                if (how.keyword == "Distribute")
                    along = len * static_cast<double>(i + 1) / n;
                else if (how.keyword == "Spacing")
                    along = spacing * static_cast<double>(i + 1);
                moveText(ctx, spots[i].id, a + u * along - spots[i].corner);
            }
            ctx.message(std::to_string(spots.size()) + " text objects aligned.");
            co_return;
        }
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
            if (!moveText(ctx, ids[i], d))
                continue;
            ++n;
        }
        ctx.message(std::to_string(n) + " text objects aligned.");
    }

private:
    static bool moveText(cmd::CommandContext& ctx, EntityId id, Vec2 d) {
        return ctx.tx()
            .modify(id,
                    [d](Entity& e) {
                        if (auto* t = entityAs<TextData>(e)) {
                            t->position += d;
                            t->alignPoint += d;
                        } else if (auto* m = entityAs<MTextData>(e)) {
                            m->position += d;
                        }
                    })
            .ok();
    }
};

}  // namespace

std::unique_ptr<cmd::Command> makeTextAlignCommand() {
    return std::make_unique<TextAlignCommand>();
}

}  // namespace tbcad::textcmd
