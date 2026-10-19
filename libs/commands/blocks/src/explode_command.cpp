#include <algorithm>
#include <cstdlib>
#include <optional>

#include "commands.h"
#include "tbcad/blocks/tbcad_blocks.h"

namespace tbcad::block_cmds {

namespace {

// EXPLODE [X]: replaces inserts and polylines with their parts, one level at a time.
class ExplodeCommand final : public cmd::Command {
public:
    // XPLODE is EXPLODE that first asks what layer and colour the parts get.
    explicit ExplodeCommand(bool xplode = false, bool burst = false)
        : xplode_(xplode), burst_(burst) {}
    std::string_view name() const override {
        return burst_ ? "BURST" : xplode_ ? "XPLODE" : "EXPLODE";
    }
    std::vector<std::string> defaultAliases() const override {
        return xplode_ || burst_ ? std::vector<std::string>{} : std::vector<std::string>{"X"};
    }
    std::string_view description() const override {
        return burst_ ? "Break blocks into parts; attributes become text with their values"
               : xplode_ ? "Break blocks and polylines into parts and set their layer and colour"
                         : "Break blocks and polylines into parts";
    }

    cmd::Task run(cmd::CommandContext& ctx) override {
        Document& doc = ctx.doc();
        auto base = detail::editableFilter(doc);
        auto sel = co_await ctx.getSelection(
            "Select objects to explode", cmd::SelectionOptions{.filter = [base, burst = burst_](const Entity& e) {
                return base(e) && blocks::canExplode(kind(e)) &&
                       (!burst || kind(e) == EntityKind::Insert);
            }});
        if (!sel.ok())
            co_return;
        if (sel.ids.empty()) {
            ctx.message("0 found. Nothing to explode was selected.");
            co_return;
        }

        LayerId layer{};
        std::optional<Color> color;
        while (xplode_) {
            cmd::KeywordOptions ko;
            ko.keywords = {"Layer", "Color", "Explode"};
            ko.defaultKeyword = "Explode";
            auto k = co_await ctx.getKeyword("Enter an option [Layer/Color/Explode] <Explode>",
                                             std::move(ko));
            if (!k.ok())
                co_return;
            if (k.keyword == "Explode")
                break;
            if (k.keyword == "Layer") {
                auto n = co_await ctx.getString("Layer name for the parts");
                if (!n.ok())
                    co_return;
                const Layer* l = doc.layerByName(cmd::trim(n.text));
                if (!l)
                    ctx.message("There is no layer named " + n.text + ".");
                else
                    layer = l->id;
            } else {
                auto n = co_await ctx.getString("Colour for the parts (1-255 or ByLayer)");
                if (!n.ok())
                    co_return;
                const std::string t(cmd::trim(n.text));
                const int idx = std::atoi(t.c_str());
                if (t == "ByLayer" || t == "bylayer")
                    color = Color::byLayer();
                else if (idx >= 1 && idx <= 255)
                    color = Color::index(static_cast<std::uint8_t>(idx));
                else
                    ctx.message("The colour is a number from 1 to 255, or ByLayer.");
            }
        }

        std::vector<EntityId> created;
        std::size_t exploded = 0;
        std::vector<std::string> notes;
        const std::vector<EntityId> ids = detail::inDrawOrder(doc, sel.ids);
        Transaction& tx = ctx.tx();
        for (EntityId id : ids) {
            const Entity* e = doc.find(id);
            if (!e || !blocks::canExplode(kind(*e)))
                continue;
            auto r = blocks::explode(doc, *e);
            if (!r) {
                ctx.message(r.error().message);
                continue;
            }
            for (std::string& m : r.value().messages)
                if (std::find(notes.begin(), notes.end(), m) == notes.end())
                    notes.push_back(std::move(m));
            (void)tx.erase(id);
            for (Entity& part : r.value().entities) {
                if (layer)
                    part.common.layer = layer;
                if (color)
                    part.common.color = *color;
                auto added = tx.add(std::move(part));
                if (added)
                    created.push_back(added.value());
                else
                    ctx.message(added.error().message);
            }
            ++exploded;
        }
        for (const std::string& m : notes)
            ctx.message(m);
        if (exploded == 0) {
            ctx.message("Nothing could be exploded");
            co_return;
        }
        ctx.host().setSelection(created);
        ctx.message("Exploded " + detail::objects(exploded) + " into " +
                    detail::objects(created.size()));
        ctx.setUndoLabel("Explode " + detail::objects(exploded));
    }

private:
    bool xplode_;
    bool burst_;
};

}  // namespace

std::unique_ptr<cmd::Command> makeExplodeCommand() {
    return std::make_unique<ExplodeCommand>();
}

std::unique_ptr<cmd::Command> makeXplodeCommand() {
    return std::make_unique<ExplodeCommand>(true);
}

std::unique_ptr<cmd::Command> makeBurstCommand() {
    return std::make_unique<ExplodeCommand>(false, true);
}

}  // namespace tbcad::block_cmds
