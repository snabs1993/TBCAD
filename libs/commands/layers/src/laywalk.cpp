#include <cstdio>
#include <map>
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

// The loops below use structured bindings, which MSVC miscompiles inside coroutines, so they live
// in plain functions.
void showOnly(CommandContext& ctx, const std::vector<LayerId>& ids) {
    const Document& doc = ctx.doc();
    const std::map<LayerId, Layer> rows = doc.layers();  // a copy: the edits below touch the rows
    // Keep the picked layers, their parents and their sub-layers; hide the rest. The
    // changes are made from the committed state, so each layer is set directly.
    std::set<std::uint32_t> keep;
    for (LayerId id : ids) {
        for (LayerId a = id; a; a = doc.layer(a) ? doc.layer(a)->parent : LayerId{})
            keep.insert(a.value);
        for (const auto& [lid, l] : rows)
            for (LayerId a = lid; a; a = doc.layer(a) ? doc.layer(a)->parent : LayerId{})
                if (a == id)
                    keep.insert(lid.value);
    }
    for (const auto& [id, l] : rows) {
        const bool show = ids.empty() || keep.count(id.value) > 0;
        if (l.on != show)
            (void)setOn(doc, ctx.tx(), id, show);
        if (show && l.frozen)
            (void)setFrozen(doc, ctx.tx(), id, false);
    }
}

void restoreState(CommandContext& ctx) {
    const Document& doc = ctx.doc();
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
        // Read both flags first: setOn may replace the row `l` points to.
        const bool wasOn = l->on, wasFrozen = l->frozen;
        if (wasOn != (on != 0))
            (void)setOn(doc, ctx.tx(), lid, on != 0);
        if (wasFrozen != (frozen != 0))
            (void)setFrozen(doc, ctx.tx(), lid, frozen != 0);
    }
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
            showOnly(ctx, ids.value());
        } else if (action.keyword == "Restore") {
            restoreState(ctx);
            ctx.host().setSetting(kStateKey, "");
        } else {
            ctx.host().setSetting(kStateKey, "");
        }
    }
};

class LayerPCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "LAYERP"; }
    std::string_view description() const override {
        return "Undo the last change to the layer settings";
    }
    Task run(CommandContext& ctx) override {
        ctx.setUndoLabel("LAYERP");
        if (auto r = restorePreviousLayers(ctx.doc(), ctx.tx()); !r) {
            ctx.message(r.error().message);
            co_return;
        }
        ctx.message("Previous layer settings restored.");
    }
};

}  // namespace

std::unique_ptr<cmd::Command> makeLayerPCommand() {
    return std::make_unique<LayerPCommand>();
}

std::unique_ptr<cmd::Command> makeLayWalkCommand() {
    return std::make_unique<LayWalkCommand>();
}
std::unique_ptr<cmd::Command> makeLayWalkStepCommand() {
    return std::make_unique<LayWalkStepCommand>();
}

}  // namespace tbcad::layers
