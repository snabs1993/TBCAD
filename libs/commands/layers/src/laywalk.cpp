#include <cstdio>
#include <set>
#include <sstream>

#include "commands.h"
#include "tbcad/layers/layer_ops.h"

namespace tbcad::layers {

namespace {

using cmd::CommandContext;
using cmd::Task;

constexpr const char* kStateKey =
    "tmp.laywalk.state";  // "<layer>:<on>:<frozen>;..." before the walk

std::string encodeState(const Document& doc) {
    std::string out;
    for (const auto& [id, l] : doc.layers())
        out += std::to_string(id.value) + ":" + (l.on ? "1" : "0") + ":" + (l.frozen ? "1" : "0") +
               ";";
    return out;
}

class LayWalkCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "LAYWALK"; }
    std::string_view description() const override {
        return "Walk through layers: show only the ones you pick, then restore";
    }
    Task run(CommandContext& ctx) override {
        ctx.host().setSetting("ui.showPanel", "laywalk");
        ctx.message("Pick the layers to show in the Layer Walk panel; Close restores them.");
        co_return;
    }
};

// -LAYWALK Show <layers> | Restore | Keep: what the Layer Walk window does to the drawing.
class LayWalkStepCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "-LAYWALK"; }
    Task run(CommandContext& ctx) override {
        cmd::KeywordOptions ko;
        ko.keywords = {"Show", "Restore", "Keep"};
        ko.defaultKeyword = "Restore";
        auto action = co_await ctx.getKeyword("Layer walk", ko);
        if (!action.ok())
            co_return;
        const Document& doc = ctx.doc();
        if (action.keyword == "Show") {
            auto text = co_await ctx.getString("Layer(s) to show");
            if (!text.ok())
                co_return;
            auto ids = resolveLayers(doc, text.text);
            if (!ids) {
                ctx.message(ids.error().message);
                co_return;
            }
            if (ctx.host().setting(kStateKey).value_or("").empty())
                ctx.host().setSetting(kStateKey, encodeState(doc));
            // Keep the picked layers, their parents and their sub-layers; hide the rest. The
            // changes are made from the committed state, so each layer is set directly.
            std::set<std::uint32_t> keep;
            for (LayerId id : ids.value()) {
                for (LayerId a = id; a; a = doc.layer(a) ? doc.layer(a)->parent : LayerId{})
                    keep.insert(a.value);
                for (const auto& [lid, l] : doc.layers())
                    for (LayerId a = lid; a; a = doc.layer(a) ? doc.layer(a)->parent : LayerId{})
                        if (a == id)
                            keep.insert(lid.value);
            }
            for (const auto& [id, l] : doc.layers()) {
                const bool show = ids.value().empty() || keep.count(id.value) > 0;
                if (l.on != show)
                    (void)setOn(doc, ctx.tx(), id, show);
                if (show && l.frozen)
                    (void)setFrozen(doc, ctx.tx(), id, false);
            }
        } else if (action.keyword == "Restore") {
            const std::string saved = ctx.host().setting(kStateKey).value_or("");
            std::istringstream in(saved);
            std::string item;
            while (std::getline(in, item, ';')) {
                unsigned id = 0, on = 1, frozen = 0;
                if (std::sscanf(item.c_str(), "%u:%u:%u", &id, &on, &frozen) != 3)
                    continue;
                const LayerId lid{id};
                const Layer* l = doc.layer(lid);
                if (!l)
                    continue;
                if (l->on != (on != 0))
                    (void)setOn(doc, ctx.tx(), lid, on != 0);
                if (l->frozen != (frozen != 0))
                    (void)setFrozen(doc, ctx.tx(), lid, frozen != 0);
            }
            ctx.host().setSetting(kStateKey, "");
        } else {
            ctx.host().setSetting(kStateKey, "");
        }
    }
};

}  // namespace

std::unique_ptr<cmd::Command> makeLayWalkCommand() {
    return std::make_unique<LayWalkCommand>();
}
std::unique_ptr<cmd::Command> makeLayWalkStepCommand() {
    return std::make_unique<LayWalkStepCommand>();
}

}  // namespace tbcad::layers
