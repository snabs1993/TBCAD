#include <cstdlib>
#include <sstream>

#include "commands.h"
#include "tbcad/layers/layer_ops.h"
#include "tbcad/layers/layer_states.h"

namespace tbcad::layers {

namespace {

using cmd::CommandContext;
using cmd::Task;

Task askText(CommandContext& ctx, std::string prompt, std::string& out, bool& ok) {
    auto r = co_await ctx.getString(std::move(prompt));
    ok = r.ok() && !r.text.empty();
    if (ok)
        out = std::move(r.text);
}

void report(CommandContext& ctx, const Result<void>& r) {
    if (!r)
        ctx.message(r.error().message);
}

std::string join(const std::vector<std::string>& v) {
    std::string out;
    for (const std::string& s : v)
        out += (out.empty() ? "" : ", ") + s;
    return out;
}

// -LAYERSTATE: Save / Restore / Delete / Rename / List.
class LayerStateCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "-LAYERSTATE"; }
    std::vector<std::string> defaultAliases() const override { return {"-LAS"}; }
    std::string_view description() const override { return "Save and restore named layer states"; }

    Task run(CommandContext& ctx) override {
        ctx.commitOnCancel();
        ctx.setUndoLabel("LAYERSTATE");
        cmd::KeywordOptions opts;
        opts.keywords = {"Save", "Restore", "Delete", "Rename", "List"};
        while (true) {
            auto k = co_await ctx.getKeyword("Layer state option", opts);
            if (!k.ok())
                co_return;
            co_await runOption(ctx, k.keyword);
        }
    }

private:
    static Task runOption(CommandContext& ctx, std::string option) {
        const Document& doc = ctx.doc();
        bool ok = false;
        if (option == "List") {
            auto names = layerStateNames(doc);
            ctx.message(names.empty() ? "No layer states saved." : "Layer states: " + join(names));
            co_return;
        }
        std::string name;
        co_await askText(ctx, "Layer state name", name, ok);
        if (!ok)
            co_return;
        if (option == "Save") {
            auto r = saveLayerState(doc, ctx.tx(), name);
            report(ctx, r);
            if (r)
                ctx.message("Saved layer state \"" + name + "\".");
        } else if (option == "Restore") {
            StateParts parts;
            cmd::KeywordOptions po;
            po.keywords = {"All", "Visibility", "Lock", "Plot", "Look"};
            po.defaultKeyword = "All";
            auto p = co_await ctx.getKeyword("What to restore", po);
            if (!p.ok())
                co_return;
            if (p.keyword != "All") {
                parts = StateParts{false, false, false, false};
                parts.visibility = p.keyword == "Visibility";
                parts.lock = p.keyword == "Lock";
                parts.plot = p.keyword == "Plot";
                parts.look = p.keyword == "Look";
            }
            std::size_t skipped = 0;
            auto r = restoreLayerState(doc, ctx.tx(), name, parts, &skipped);
            report(ctx, r);
            if (r)
                ctx.message("Restored layer state \"" + name + "\"" +
                            (skipped ? "; " + std::to_string(skipped) +
                                           " layer(s) were missing or could not be set."
                                     : "."));
        } else if (option == "Delete") {
            report(ctx, deleteLayerState(doc, ctx.tx(), name));
        } else if (option == "Rename") {
            std::string to;
            co_await askText(ctx, "New name", to, ok);
            if (!ok)
                co_return;
            report(ctx, renameLayerState(doc, ctx.tx(), name, to));
        }
    }
};

// Sets the active filter and bumps the serial so a panel notices even a repeated value.
void setActiveFilter(CommandContext& ctx, const std::string& expression) {
    ctx.host().setSetting("layers.filter", expression);
    long serial = std::atol(ctx.host().setting("layers.filter.serial").value_or("0").c_str());
    ctx.host().setSetting("layers.filter.serial", std::to_string(serial + 1));
}

// -LAYERFILTER: Apply / Save / Delete / Clear / List. The active filter is the host setting
// `layers.filter`; the Layers panel shows only the layers that match it.
class LayerFilterCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "-LAYERFILTER"; }
    std::vector<std::string> defaultAliases() const override { return {"-LAF"}; }
    std::string_view description() const override {
        return "Filter the layer list by name, switches, look or branch";
    }

    Task run(CommandContext& ctx) override {
        ctx.commitOnCancel();
        ctx.setUndoLabel("LAYERFILTER");
        cmd::KeywordOptions opts;
        opts.keywords = {"Apply", "Save", "Use", "Delete", "Clear", "List"};
        while (true) {
            auto k = co_await ctx.getKeyword("Layer filter option", opts);
            if (!k.ok())
                co_return;
            co_await runOption(ctx, k.keyword);
        }
    }

private:
    static void show(CommandContext& ctx, const std::string& expression) {
        auto ids = filterLayers(ctx.doc(), expression);
        if (!ids) {
            ctx.message(ids.error().message);
            return;
        }
        setActiveFilter(ctx, expression);
        std::string list;
        for (LayerId id : ids.value())
            list += (list.empty() ? "" : ", ") + layerPath(ctx.doc(), id);
        ctx.message(std::to_string(ids.value().size()) + " layer(s) match" +
                    (list.empty() ? "." : ": " + list));
    }

    static Task runOption(CommandContext& ctx, std::string option) {
        const Document& doc = ctx.doc();
        bool ok = false;
        if (option == "List") {
            auto names = layerFilterNames(doc);
            ctx.message(names.empty() ? "No layer filters saved."
                                      : "Layer filters: " + join(names));
            co_return;
        }
        if (option == "Clear") {
            setActiveFilter(ctx, "");
            ctx.message("Layer filter cleared.");
            co_return;
        }
        if (option == "Apply") {
            std::string expr;
            co_await askText(ctx, "Filter (e.g. name:S-* frozen, not:used, branch:Walls)", expr,
                             ok);
            if (ok)
                show(ctx, expr);
            co_return;
        }
        std::string name;
        co_await askText(ctx, "Layer filter name", name, ok);
        if (!ok)
            co_return;
        if (option == "Use") {
            auto e = savedFilterExpression(doc, name);
            if (!e)
                ctx.message(e.error().message);
            else
                show(ctx, e.value());
        } else if (option == "Save") {
            std::string expr;
            co_await askText(ctx, "Filter expression", expr, ok);
            if (!ok)
                co_return;
            auto r = saveLayerFilter(doc, ctx.tx(), name, expr);
            report(ctx, r);
            if (r)
                ctx.message("Saved layer filter \"" + name + "\".");
        } else if (option == "Delete") {
            report(ctx, deleteLayerFilter(doc, ctx.tx(), name));
        }
    }
};

}  // namespace

std::unique_ptr<cmd::Command> makeLayerStateCommand() {
    return std::make_unique<LayerStateCommand>();
}
std::unique_ptr<cmd::Command> makeLayerFilterCommand() {
    return std::make_unique<LayerFilterCommand>();
}

}  // namespace tbcad::layers
