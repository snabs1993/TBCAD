#include <cctype>
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
            po.keywords = {"All", "Visibility", "Lock", "Plot", "Look", "HideOthers", "Custom"};
            po.defaultKeyword = "All";
            auto p = co_await ctx.getKeyword("What to restore", po);
            if (!p.ok())
                co_return;
            if (p.keyword == "Custom") {
                // Several parts at once (the Layer States dialog): words from Visibility, Lock,
                // Plot, Look, Current and HideOthers, separated by spaces or commas.
                auto t = co_await ctx.getString(
                    "Parts [Visibility Lock Plot Look Current HideOthers], separated by spaces");
                if (!t.ok())
                    co_return;
                parts = StateParts{false, false, false, false, false, false};
                std::string word;
                auto take = [&] {
                    std::string w;
                    for (char c : word)
                        w += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                    if (w == "visibility")
                        parts.visibility = true;
                    else if (w == "lock")
                        parts.lock = true;
                    else if (w == "plot")
                        parts.plot = true;
                    else if (w == "look")
                        parts.look = true;
                    else if (w == "current")
                        parts.current = true;
                    else if (w == "hideothers")
                        parts.hideOthers = true;
                    word.clear();
                };
                for (char c : t.text) {
                    if (c == ' ' || c == ',')
                        take();
                    else
                        word += c;
                }
                take();
            } else if (p.keyword == "HideOthers")
                parts.hideOthers = true;
            else if (p.keyword != "All") {
                parts = StateParts{false, false, false, false};
                parts.visibility = p.keyword == "Visibility";
                parts.lock = p.keyword == "Lock";
                parts.plot = p.keyword == "Plot";
                parts.look = p.keyword == "Look";
                parts.current = false;
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

std::vector<std::string> splitList(const std::string& text) {
    std::vector<std::string> out;
    std::size_t pos = 0;
    while (pos <= text.size()) {
        std::size_t next = text.find(',', pos);
        std::string part =
            cmd::trim(text.substr(pos, next == std::string::npos ? std::string::npos : next - pos));
        if (!part.empty())
            out.push_back(part);
        if (next == std::string::npos)
            break;
        pos = next + 1;
    }
    return out;
}

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
        opts.keywords = {"Apply", "Save",  "Use",   "Delete",     "Clear",
                         "List",  "Group", "AddTo", "RemoveFrom", "DeleteGroup"};
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
            auto groups = layerGroupNames(doc);
            ctx.message(groups.empty() ? "No layer groups saved."
                                       : "Layer groups: " + join(groups));
            co_return;
        }
        if (option == "Group" || option == "AddTo" || option == "RemoveFrom" ||
            option == "DeleteGroup") {
            std::string gname;
            co_await askText(ctx, "Layer group name", gname, ok);
            if (!ok)
                co_return;
            if (option == "DeleteGroup") {
                report(ctx, deleteLayerGroup(doc, ctx.tx(), gname));
                co_return;
            }
            std::string list;
            co_await askText(ctx, "Layers, separated by commas", list, ok);
            if (!ok)
                co_return;
            std::vector<LayerId> ids;
            for (const std::string& part : splitList(list)) {
                auto id = resolveLayer(doc, part);
                if (!id) {
                    ctx.message(id.error().message);
                    co_return;
                }
                ids.push_back(id.value());
            }
            Result<void> r = option == "Group"   ? saveLayerGroup(doc, ctx.tx(), gname, ids)
                             : option == "AddTo" ? addToLayerGroup(doc, ctx.tx(), gname, ids)
                                                 : removeFromLayerGroup(doc, ctx.tx(), gname, ids);
            report(ctx, r);
            if (r)
                ctx.message("Layer group \"" + gname + "\" updated.");
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
