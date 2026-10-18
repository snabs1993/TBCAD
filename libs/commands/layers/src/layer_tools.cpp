// One-step layer tools that work from picked objects: LAYOFF, LAYFRZ, LAYLCK, LAYULK, LAYISO,
// LAYUNISO, LAYON, LAYTHW, LAYMCH, LAYMRG, LAYDEL.
#include <algorithm>
#include <sstream>

#include "commands.h"
#include "tbcad/layers/layer_ops.h"

namespace tbcad::layers {

namespace {

using cmd::CommandContext;
using cmd::Task;

constexpr const char* kIsoKey = "tbcad.layiso";  // lines "layerId\ton" saved by LAYISO

std::vector<LayerId> layersOf(const Document& doc, const std::vector<EntityId>& ids) {
    std::vector<LayerId> out;
    for (EntityId id : ids)
        if (const Entity* e = doc.find(id))
            if (std::find(out.begin(), out.end(), e->common.layer) == out.end())
                out.push_back(e->common.layer);
    return out;
}

std::string names(const Document& doc, const std::vector<LayerId>& ids) {
    std::string s;
    for (LayerId id : ids)
        s += (s.empty() ? "" : ", ") + layerPath(doc, id);
    return s;
}

enum class Act { Off, Freeze, Lock, Unlock };

class PickedLayerCommand final : public cmd::Command {
public:
    PickedLayerCommand(const char* name, const char* description, Act act)
        : name_(name), description_(description), act_(act) {}
    std::string_view name() const override { return name_; }
    std::string_view description() const override { return description_; }

    Task run(CommandContext& ctx) override {
        auto sel = co_await ctx.getSelection("Select objects on the layers to change");
        if (!sel.ok())
            co_return;
        std::vector<LayerId> ids = layersOf(ctx.doc(), sel.ids);
        std::vector<LayerId> done;
        for (LayerId id : ids) {
            Result<void> r;
            switch (act_) {
                case Act::Off:
                    r = setOn(ctx.doc(), ctx.tx(), id, false);
                    break;
                case Act::Freeze:
                    r = setFrozen(ctx.doc(), ctx.tx(), id, true);
                    break;
                case Act::Lock:
                    r = setLocked(ctx.doc(), ctx.tx(), id, true);
                    break;
                case Act::Unlock:
                    r = setLocked(ctx.doc(), ctx.tx(), id, false);
                    break;
            }
            if (r)
                done.push_back(id);
            else
                ctx.message(r.error().message);
        }
        if (!done.empty())
            ctx.message(std::string(description_) + ": " + names(ctx.doc(), done) + ".");
    }

private:
    const char* name_;
    const char* description_;
    Act act_;
};

class LayIsoCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "LAYISO"; }
    std::string_view description() const override {
        return "Hide every layer except those of the selected objects";
    }
    Task run(CommandContext& ctx) override {
        auto sel = co_await ctx.getSelection("Select objects on the layers to isolate");
        if (!sel.ok())
            co_return;
        std::vector<LayerId> keep = layersOf(ctx.doc(), sel.ids);
        if (keep.empty())
            co_return;
        // Remember which layers were on so LAYUNISO can bring them back (kept across repeats).
        std::string saved;
        if (!ctx.doc().header().custom.count(kIsoKey))
            for (const auto& [id, l] : ctx.doc().layers())
                saved += std::to_string(id.value) + "\t" + (l.on ? "1" : "0") + "\n";
        auto r = isolateLayers(ctx.doc(), ctx.tx(), keep);
        if (!r) {
            ctx.message(r.error().message);
            co_return;
        }
        if (!saved.empty())
            (void)ctx.tx().modifyHeader([&](Header& h) { h.custom[kIsoKey] = saved; });
        ctx.message("Isolated: " + names(ctx.doc(), keep) + ".");
    }
};

class LayUnisoCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "LAYUNISO"; }
    std::string_view description() const override { return "Restore the layers hidden by LAYISO"; }
    Task run(CommandContext& ctx) override {
        auto it = ctx.doc().header().custom.find(kIsoKey);
        if (it == ctx.doc().header().custom.end()) {
            ctx.message("No layers are isolated.");
            co_return;
        }
        std::istringstream in(it->second);
        std::string line;
        while (std::getline(in, line)) {
            auto tab = line.find('\t');
            if (tab == std::string::npos)
                continue;
            LayerId id{static_cast<std::uint32_t>(std::stoul(line.substr(0, tab)))};
            if (ctx.doc().layer(id))
                (void)setOn(ctx.doc(), ctx.tx(), id, line.substr(tab + 1) == "1");
        }
        (void)ctx.tx().modifyHeader([](Header& h) { h.custom.erase(kIsoKey); });
        ctx.message("Layers restored.");
    }
};

class LayOnCommand final : public cmd::Command {
public:
    LayOnCommand(const char* name, const char* description, bool thaw)
        : name_(name), description_(description), thaw_(thaw) {}
    std::string_view name() const override { return name_; }
    std::string_view description() const override { return description_; }
    Task run(CommandContext& ctx) override {
        std::vector<LayerId> ids;
        for (const auto& [id, l] : ctx.doc().layers())
            if (thaw_ ? l.frozen : !l.on)
                ids.push_back(id);
        for (LayerId id : ids)
            (void)(thaw_ ? setFrozen(ctx.doc(), ctx.tx(), id, false)
                         : setOn(ctx.doc(), ctx.tx(), id, true));
        ctx.message(ids.empty() ? "Nothing to change."
                                : std::to_string(ids.size()) +
                                      (thaw_ ? " layer(s) thawed." : " layer(s) turned on."));
        co_return;
    }

private:
    const char* name_;
    const char* description_;
    bool thaw_;
};

class LayMchCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "LAYMCH"; }
    std::string_view description() const override {
        return "Move objects to the layer of another object";
    }
    Task run(CommandContext& ctx) override {
        auto sel = co_await ctx.getSelection("Select objects to change");
        if (!sel.ok())
            co_return;
        auto dest = co_await ctx.getEntity("Select an object on the destination layer");
        if (!dest.ok())
            co_return;
        const Entity* d = ctx.doc().find(dest.id);
        if (!d)
            co_return;
        LayerId to = d->common.layer;
        std::size_t n = 0;
        for (EntityId id : sel.ids)
            if (ctx.tx().modify(id, [&](Entity& e) { e.common.layer = to; }))
                ++n;
        ctx.message(std::to_string(n) + " object(s) moved to " + layerPath(ctx.doc(), to) + ".");
    }
};

class LayMrgCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "LAYMRG"; }
    std::string_view description() const override {
        return "Merge the layers of the selected objects into another layer";
    }
    Task run(CommandContext& ctx) override {
        auto sel = co_await ctx.getSelection("Select objects on the layers to merge away");
        if (!sel.ok())
            co_return;
        auto dest = co_await ctx.getEntity("Select an object on the target layer");
        if (!dest.ok())
            co_return;
        const Entity* d = ctx.doc().find(dest.id);
        if (!d)
            co_return;
        LayerId to = d->common.layer;
        for (LayerId from : layersOf(ctx.doc(), sel.ids)) {
            if (from == to)
                continue;
            std::string path = layerPath(ctx.doc(), from);
            auto r = mergeLayer(ctx.doc(), ctx.tx(), from, to);
            ctx.message(r ? "Merged " + path + " into " + layerPath(ctx.doc(), to) + "."
                          : r.error().message);
        }
    }
};

class LayDelCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "LAYDEL"; }
    std::string_view description() const override {
        return "Delete the layer of an object together with everything on it";
    }
    Task run(CommandContext& ctx) override {
        auto e = co_await ctx.getEntity("Select an object on the layer to delete");
        if (!e.ok())
            co_return;
        const Entity* ent = ctx.doc().find(e.id);
        if (!ent)
            co_return;
        LayerId id = ent->common.layer;
        std::string path = layerPath(ctx.doc(), id);
        if (id == ctx.doc().header().currentLayer) {
            ctx.message("Cannot delete the current layer.");
            co_return;
        }
        cmd::KeywordOptions ko;
        ko.keywords = {"Yes", "No"};
        ko.defaultKeyword = "No";
        auto k = co_await ctx.getKeyword("Delete layer " + path + " and all its objects", ko);
        if (!k.ok() || k.keyword != "Yes")
            co_return;
        std::vector<EntityId> doomed;
        ctx.doc().forEachEntity([&](const Entity& x) {
            if (x.common.layer == id)
                doomed.push_back(x.common.id);
        });
        for (EntityId x : doomed)
            (void)ctx.tx().erase(x);
        auto r = deleteLayer(ctx.doc(), ctx.tx(), id, DeleteMode::LeafOnly);
        if (!r) {
            ctx.message(r.error().message);
            co_return;
        }
        ctx.message("Deleted layer " + path + " and " + std::to_string(doomed.size()) +
                    " object(s).");
    }
};

}  // namespace

// DIMLAYER, HPLAYER and TEXTLAYER: the layer new dimensions (with leaders), hatches or text go on.
// "." goes back to drawing on the current layer.
class CommandLayerCommand final : public cmd::Command {
public:
    CommandLayerCommand(const char* name, const char* what, const char* key)
        : name_(name), what_(what), key_(key) {}
    std::string_view name() const override { return name_; }
    std::string_view description() const override { return description_; }

    Task run(CommandContext& ctx) override {
        std::string now = ".";
        std::string shown = "current layer";
        auto it = ctx.doc().header().custom.find(key_);
        if (it != ctx.doc().header().custom.end()) {
            const LayerId id{
                static_cast<std::uint32_t>(std::strtoul(it->second.c_str(), nullptr, 10))};
            if (ctx.doc().layer(id)) {
                now = layerPath(ctx.doc(), id);
                shown = now;
            }
        }
        cmd::StringOptions so;
        so.defaultValue = now;
        auto r = co_await ctx.getString(
            std::string("Layer for new ") + what_ + " (. = current layer) <" + now + ">", so);
        if (!r.ok())
            co_return;
        std::string name = cmd::trim(r.text.empty() ? now : r.text);
        if (name == ".") {
            (void)ctx.tx().modifyHeader([k = key_](Header& h) { h.custom.erase(k); });
            ctx.setUndoLabel(name_);
            ctx.message(std::string("New ") + what_ + " go on the current layer.");
            co_return;
        }
        const Layer* layer = ctx.doc().layerByName(name);
        if (!layer) {
            ctx.message("There is no layer named \"" + name + "\".");
            co_return;
        }
        (void)ctx.tx().modifyHeader(
            [k = key_, id = layer->id](Header& h) { h.custom[k] = std::to_string(id.value); });
        ctx.setUndoLabel(name_);
        ctx.message(std::string("New ") + what_ + " go on layer \"" + layer->name + "\".");
    }

private:
    const char* name_;
    const char* what_;
    const char* key_;
    std::string description_ = std::string("Choose the layer for new ") + what_;
};

void registerLayerTools(cmd::Registry& registry) {
    (void)registry.add(std::make_unique<PickedLayerCommand>("LAYOFF", "Turned off", Act::Off));
    (void)registry.add(std::make_unique<PickedLayerCommand>("LAYFRZ", "Froze", Act::Freeze));
    (void)registry.add(std::make_unique<PickedLayerCommand>("LAYLCK", "Locked", Act::Lock));
    (void)registry.add(std::make_unique<PickedLayerCommand>("LAYULK", "Unlocked", Act::Unlock));
    (void)registry.add(std::make_unique<LayIsoCommand>());
    (void)registry.add(std::make_unique<LayUnisoCommand>());
    (void)registry.add(std::make_unique<LayOnCommand>("LAYON", "Turn on all layers", false));
    (void)registry.add(std::make_unique<LayOnCommand>("LAYTHW", "Thaw all layers", true));
    (void)registry.add(std::make_unique<LayMchCommand>());
    (void)registry.add(std::make_unique<LayMrgCommand>());
    (void)registry.add(std::make_unique<LayDelCommand>());
    (void)registry.add(makeLayTransCommand());
    (void)registry.add(
        std::make_unique<CommandLayerCommand>("DIMLAYER", "dimensions and leaders", kDimLayerKey));
    (void)registry.add(std::make_unique<CommandLayerCommand>("HPLAYER", "hatches", kHatchLayerKey));
    (void)registry.add(std::make_unique<CommandLayerCommand>("TEXTLAYER", "text", kTextLayerKey));
}

}  // namespace tbcad::layers
