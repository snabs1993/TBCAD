#include <algorithm>
#include <sstream>

#include "commands.h"
#include "tbcad/layers/layer_ops.h"
#include "tbcad/layers/linetype_file.h"

namespace tbcad::layers {

namespace {

using cmd::CommandContext;
using cmd::Task;

// Asks for a line of text. `ok` is false on Esc / empty answer.
Task askText(CommandContext& ctx, std::string prompt, std::string& out, bool& ok) {
    auto r = co_await ctx.getString(std::move(prompt));
    ok = r.ok() && !r.text.empty();
    if (ok)
        out = std::move(r.text);
}

// Asks for a layer list and resolves it (reports the problem itself).
Task askLayers(CommandContext& ctx, std::string prompt, std::vector<LayerId>& out, bool& ok) {
    std::string text;
    co_await askText(ctx, std::move(prompt), text, ok);
    if (!ok)
        co_return;
    auto r = resolveLayers(ctx.doc(), text);
    if (!r) {
        ctx.message(r.error().message);
        ok = false;
        co_return;
    }
    out = std::move(r.value());
}

std::vector<std::string> splitNames(const std::string& text) {
    std::vector<std::string> out;
    std::istringstream in(text);
    std::string part;
    while (std::getline(in, part, ',')) {
        std::size_t b = part.find_first_not_of(" \t");
        std::size_t e = part.find_last_not_of(" \t");
        if (b != std::string::npos)
            out.push_back(part.substr(b, e - b + 1));
    }
    return out;
}

void report(CommandContext& ctx, const Result<void>& r) {
    if (!r)
        ctx.message(r.error().message);
}

class LayerPanelCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "LAYER"; }
    std::vector<std::string> defaultAliases() const override { return {"LA"}; }
    std::string_view description() const override { return "Open the layer panel"; }
    Task run(CommandContext& ctx) override {
        // The application watches this key and opens (or focuses) the panel.
        ctx.host().setSetting("ui.showPanel", "layers");
        ctx.message("Layer panel. Use -LAYER to change layers from the command line.");
        co_return;
    }
};

class LayerCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "-LAYER"; }
    std::vector<std::string> defaultAliases() const override { return {"-LA"}; }
    std::string_view description() const override {
        return "Create and change layers and sub-layers from the command line";
    }

    Task run(CommandContext& ctx) override {
        ctx.commitOnCancel();
        ctx.setUndoLabel("LAYER");
        cmd::KeywordOptions opts;
        opts.keywords = {"Make",     "New",        "NewSub", "Parent", "Set",    "On",
                         "Off",      "Freeze",     "Thaw",   "Lock",   "Unlock", "Color",
                         "Linetype", "Lineweight", "Plot",   "NoPlot", "Rename", "Delete",
                         "Isolate",  "ShowAll",    "List"};
        opts.aliases = {{"N", "New"}, {"S", "Set"}};
        while (true) {
            auto k = co_await ctx.getKeyword("Layer option", opts);
            if (!k.ok())
                co_return;
            co_await runOption(ctx, k.keyword);
        }
    }

private:
    static Task runOption(CommandContext& ctx, std::string option) {
        Document& doc = ctx.doc();
        bool ok = false;
        if (option == "Make") {
            // AutoCAD's -LAYER Make: use the layer if it exists, otherwise create it, and make it
            // the current layer.
            std::string name;
            co_await askText(ctx, "Name of the layer to make current", name, ok);
            if (!ok)
                co_return;
            auto existing = resolveLayer(doc, name);
            LayerId id{};
            if (existing) {
                id = existing.value();
            } else {
                auto created = createLayer(doc, ctx.tx(), cmd::trim(name));
                if (!created) {
                    ctx.message(created.error().message);
                    co_return;
                }
                id = created.value();
                ctx.message("Created layer " + layerPath(doc, id) + ".");
            }
            report(ctx, setCurrentLayer(doc, ctx.tx(), id));
        } else if (option == "New") {
            std::string names;
            co_await askText(ctx, "Name(s) for new layer(s), separated by commas", names, ok);
            if (!ok)
                co_return;
            for (const std::string& n : splitNames(names)) {
                auto id = createLayer(doc, ctx.tx(), n);
                if (id)
                    ctx.message("Created layer " + layerPath(doc, id.value()) + ".");
                else
                    ctx.message(id.error().message);
            }
        } else if (option == "NewSub") {
            std::vector<LayerId> parents;
            co_await askLayers(ctx, "Parent layer", parents, ok);
            if (!ok)
                co_return;
            if (parents.size() != 1) {
                ctx.message("Give exactly one parent layer.");
                co_return;
            }
            std::string names;
            co_await askText(ctx, "Name(s) for new sub-layer(s), separated by commas", names, ok);
            if (!ok)
                co_return;
            for (const std::string& n : splitNames(names)) {
                auto id = createLayer(doc, ctx.tx(), n, parents[0]);
                if (id)
                    ctx.message("Created sub-layer " + layerPath(doc, id.value()) + ".");
                else
                    ctx.message(id.error().message);
            }
        } else if (option == "Parent") {
            std::vector<LayerId> layersToMove;
            co_await askLayers(ctx, "Layer(s) to move", layersToMove, ok);
            if (!ok)
                co_return;
            std::string target;
            co_await askText(ctx, "New parent layer (None = top level)", target, ok);
            if (!ok)
                co_return;
            LayerId parent{};
            if (cmd::toUpperAscii(target) != "NONE") {
                auto p = resolveLayer(doc, target);
                if (!p) {
                    ctx.message(p.error().message);
                    co_return;
                }
                parent = p.value();
            }
            for (LayerId id : layersToMove) {
                std::string before = layerPath(doc, id);
                auto r = reparentLayer(doc, ctx.tx(), id, parent);
                if (r)
                    ctx.message("Moved " + before + " to " + layerPath(doc, id) + ".");
                else
                    ctx.message(r.error().message);
            }
        } else if (option == "Set") {
            std::vector<LayerId> ids;
            co_await askLayers(ctx, "Layer to make current", ids, ok);
            if (!ok)
                co_return;
            if (ids.size() != 1) {
                ctx.message("Give exactly one layer.");
                co_return;
            }
            report(ctx, setCurrentLayer(doc, ctx.tx(), ids[0]));
        } else if (option == "On" || option == "Off" || option == "Freeze" || option == "Thaw" ||
                   option == "Lock" || option == "Unlock" || option == "Plot" ||
                   option == "NoPlot") {
            std::vector<LayerId> ids;
            co_await askLayers(ctx, "Layer name(s), * for all", ids, ok);
            if (!ok)
                co_return;
            for (LayerId id : ids) {
                Result<void> r;
                if (option == "On")
                    r = setOn(doc, ctx.tx(), id, true);
                else if (option == "Off")
                    r = setOn(doc, ctx.tx(), id, false);
                else if (option == "Freeze")
                    r = setFrozen(doc, ctx.tx(), id, true);
                else if (option == "Thaw")
                    r = setFrozen(doc, ctx.tx(), id, false);
                else if (option == "Lock")
                    r = setLocked(doc, ctx.tx(), id, true);
                else if (option == "Unlock")
                    r = setLocked(doc, ctx.tx(), id, false);
                else if (option == "Plot")
                    r = setPlot(doc, ctx.tx(), id, true);
                else
                    r = setPlot(doc, ctx.tx(), id, false);
                report(ctx, r);
            }
        } else if (option == "Color" || option == "Linetype" || option == "Lineweight") {
            std::string value;
            co_await askText(ctx,
                             option == "Color"      ? "Colour (name, 1-255, r,g,b or ByParent)"
                             : option == "Linetype" ? "Linetype name (or ByParent)"
                                                    : "Lineweight in mm (Default or ByParent)",
                             value, ok);
            if (!ok)
                co_return;
            std::vector<LayerId> ids;
            co_await askLayers(ctx, "Layer name(s), * for all", ids, ok);
            if (!ok)
                co_return;
            if (option == "Color") {
                auto c = parseColor(value);
                if (!c) {
                    ctx.message(c.error().message);
                    co_return;
                }
                for (LayerId id : ids)
                    report(ctx, setLayerColor(doc, ctx.tx(), id, c.value()));
            } else if (option == "Linetype") {
                auto lt = resolveLinetype(doc, ctx.tx(), value);
                if (!lt) {
                    ctx.message(lt.error().message);
                    co_return;
                }
                for (LayerId id : ids)
                    report(ctx, setLayerLinetype(doc, ctx.tx(), id, lt.value()));
            } else {
                auto lw = parseLineweight(value);
                if (!lw) {
                    ctx.message(lw.error().message);
                    co_return;
                }
                for (LayerId id : ids)
                    report(ctx, setLayerLineweight(doc, ctx.tx(), id, lw.value()));
            }
        } else if (option == "Rename") {
            std::vector<LayerId> ids;
            co_await askLayers(ctx, "Layer to rename", ids, ok);
            if (!ok)
                co_return;
            if (ids.size() != 1) {
                ctx.message("Give exactly one layer.");
                co_return;
            }
            std::string name;
            co_await askText(ctx, "New name", name, ok);
            if (!ok)
                co_return;
            report(ctx, renameLayer(doc, ctx.tx(), ids[0], name));
        } else if (option == "Delete") {
            std::vector<LayerId> ids;
            co_await askLayers(ctx, "Layer to delete", ids, ok);
            if (!ok)
                co_return;
            if (ids.size() != 1) {
                ctx.message("Give exactly one layer.");
                co_return;
            }
            DeleteMode mode = DeleteMode::LeafOnly;
            if (!childrenOf(doc, ids[0]).empty()) {
                cmd::KeywordOptions ko;
                ko.keywords = {"Branch", "MoveUp"};
                auto k = co_await ctx.getKeyword(
                    "Layer has sub-layers: delete the whole [Branch] or [MoveUp] the sub-layers",
                    ko);
                if (!k.ok())
                    co_return;
                mode = k.keyword == "Branch" ? DeleteMode::Branch : DeleteMode::MoveChildrenUp;
            }
            std::string path = layerPath(doc, ids[0]);
            auto r = deleteLayer(doc, ctx.tx(), ids[0], mode);
            if (r)
                ctx.message("Deleted layer " + path + ".");
            else
                ctx.message(r.error().message);
        } else if (option == "Isolate") {
            std::vector<LayerId> ids;
            co_await askLayers(ctx, "Layer(s) to isolate", ids, ok);
            if (!ok)
                co_return;
            report(ctx, isolateLayers(doc, ctx.tx(), ids));
        } else if (option == "ShowAll") {
            report(ctx, showAllLayers(doc, ctx.tx()));
        } else if (option == "List") {
            for (LayerId id : layerTreeOrder(doc)) {
                const Layer* l = doc.layer(id);
                EffectiveLayer e = doc.effective(id);
                std::string line(static_cast<std::size_t>(layerDepth(doc, id)) * 2, ' ');
                line += l->name;
                line += id == doc.header().currentLayer ? "  [current]" : "";
                line += "  " + colorName(e.color) + ", " + linetypeName(doc, e.linetype) + ", " +
                        lineweightName(e.lineweight);
                line += l->on ? "" : "  off";
                line += l->frozen ? "  frozen" : "";
                line += l->locked ? "  locked" : "";
                line += l->plot ? "" : "  no-plot";
                if (e.on != l->on || e.frozen != l->frozen || e.locked != l->locked)
                    line += "  (inherited state: " + std::string(e.visible() ? "shown" : "hidden") +
                            (e.locked ? ", locked" : "") + ")";
                ctx.message(line);
            }
        }
    }
};

class LayerCurrentCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "LAYERCURRENT"; }
    std::vector<std::string> defaultAliases() const override { return {"LAYCUR"}; }
    std::string_view description() const override {
        return "Make the layer of the picked object current";
    }
    Task run(CommandContext& ctx) override {
        auto e = co_await ctx.getEntity("Select an object to make its layer current");
        if (!e.ok())
            co_return;
        const Entity* ent = ctx.doc().find(e.id);
        if (!ent)
            co_return;
        auto r = setCurrentLayer(ctx.doc(), ctx.tx(), ent->common.layer);
        if (r)
            ctx.message("Current layer: " + layerPath(ctx.doc(), ent->common.layer) + ".");
        else
            ctx.message(r.error().message);
    }
};

class ActiveOnlyCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "ACTIVEONLY"; }
    std::vector<std::string> defaultAliases() const override { return {"AO"}; }
    std::string_view description() const override {
        return "Draw, pick and edit only on the current layer (and its sub-layers)";
    }
    Task run(CommandContext& ctx) override {
        ctx.setUndoLabel("ACTIVEONLY");
        // Free text, so typing the command or its alias again at the prompt toggles, like Enter.
        cmd::StringOptions so;
        so.allowEmpty = true;
        so.defaultValue = "Toggle";
        std::string text;
        for (;;) {
            auto k = co_await ctx.getString("Active only [On/Off/Toggle] <Toggle>", so);
            if (!k.ok())
                co_return;
            text = cmd::trim(k.text);
            for (char& c : text)
                c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            if (text.empty() || text == "T" || text == "TOGGLE" || text == "AO" ||
                text == "ACTIVEONLY" || text == "ON" || text == "OFF")
                break;
            ctx.message("Type On, Off or Toggle.");
        }
        bool on = text == "ON" ? true : text == "OFF" ? false : !ctx.doc().header().activeOnly;
        auto r = setActiveOnly(ctx.doc(), ctx.tx(), on);
        if (!r) {
            ctx.message(r.error().message);
            co_return;
        }
        ctx.message(on ? "Active Only is on: only layer " +
                             layerPath(ctx.doc(), ctx.doc().header().currentLayer) +
                             " (and its sub-layers) can be picked or edited."
                       : "Active Only is off.");
    }
};

}  // namespace

std::unique_ptr<cmd::Command> makeLayerPanelCommand() {
    return std::make_unique<LayerPanelCommand>();
}
std::unique_ptr<cmd::Command> makeLayerCommand() {
    return std::make_unique<LayerCommand>();
}
std::unique_ptr<cmd::Command> makeActiveOnlyCommand() {
    return std::make_unique<ActiveOnlyCommand>();
}
std::unique_ptr<cmd::Command> makeLayerCurrentCommand() {
    return std::make_unique<LayerCurrentCommand>();
}

}  // namespace tbcad::layers
