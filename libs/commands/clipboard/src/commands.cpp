#include <algorithm>
#include <filesystem>

#include "tbcad/clipboard/clipboard.h"
#include "tbcad/io/tbcad.h"
#include "tbcad/ui/ui_registry.h"

namespace tbcad::clipboard {

namespace {

bool visibleEntity(const Document& doc, const Entity& e) {
    if (!e.common.visible)
        return false;
    return doc.effective(e.common).live();
}

bool unlockedEntity(const Document& doc, const Entity& e) {
    return visibleEntity(doc, e) && !doc.effective(e.common).locked;
}

std::string plural(std::size_t n) {
    return std::to_string(n) + (n == 1 ? " object" : " objects");
}

class CopyCommand final : public cmd::Command {
public:
    CopyCommand(std::string name, bool withBase, bool cut)
        : name_(std::move(name)), withBase_(withBase), cut_(cut) {}

    std::string_view name() const override { return name_; }
    std::string_view description() const override {
        if (cut_)
            return "Move the selected objects to the clipboard";
        return withBase_ ? "Copy objects to the clipboard with a base point"
                         : "Copy objects to the clipboard";
    }

    cmd::Task run(cmd::CommandContext& ctx) override {
        cmd::SelectionOptions so;
        so.filter = [&doc = ctx.doc(), cut = cut_](const Entity& e) {
            return cut ? unlockedEntity(doc, e) : visibleEntity(doc, e);
        };
        auto sel = co_await ctx.getSelection(
            cut_ ? "Select objects to cut" : "Select objects to copy", so);
        if (!sel.ok())
            co_return;
        std::optional<Vec2> base;
        if (withBase_) {
            auto b = co_await ctx.getPoint("Specify base point");
            if (!b.ok())
                co_return;
            base = b.point;
        }
        Clip clip = capture(ctx.doc(), sel.ids, base);
        if (clip.empty()) {
            ctx.message("Nothing to copy.");
            co_return;
        }
        std::size_t n = clip.entities.size();
        setCurrent(std::move(clip));
        // Replaces stale text on the system clipboard, so Ctrl+V pastes these objects.
        ctx.host().setSetting("clipboard.text", "[TBCAD objects]");
        if (cut_) {
            for (EntityId id : sel.ids)
                (void)ctx.tx().erase(id);
            ctx.host().setSelection({});
            ctx.message(plural(n) + " cut.");
        } else {
            ctx.message(plural(n) + " copied.");
        }
    }

private:
    std::string name_;
    bool withBase_, cut_;
};

class PasteCommand final : public cmd::Command {
public:
    enum class Mode { Normal, Block, Original };
    PasteCommand(std::string name, Mode mode) : name_(std::move(name)), mode_(mode) {}

    std::string_view name() const override { return name_; }
    std::string_view description() const override {
        switch (mode_) {
            case Mode::Block:
                return "Paste the clipboard as one block";
            case Mode::Original:
                return "Paste the clipboard at its original coordinates";
            default:
                return "Paste the clipboard";
        }
    }

    cmd::Task run(cmd::CommandContext& ctx) override {
        const Clip& clip = current();
        if (clip.empty()) {
            ctx.message("The clipboard is empty.");
            co_return;
        }
        Vec2 target = clip.base;
        if (mode_ != Mode::Original) {
            cmd::PointOptions po;
            po.base = clip.base;
            po.preview = [&clip](Vec2 c, DisplayList& out) {
                if (clip.bounds.empty())
                    return;
                Vec2 d = c - clip.base;
                DisplayPolyline pl;
                pl.closed = true;
                pl.style.color = Rgba{255, 255, 255, 255};
                const BBox2& b = clip.bounds;
                for (Vec2 p : {b.min, Vec2{b.max.x, b.min.y}, b.max, Vec2{b.min.x, b.max.y}})
                    pl.pts.push_back(p + d);
                out.items.emplace_back(std::move(pl));
            };
            auto at = co_await ctx.getPoint("Specify insertion point", po);
            if (!at.ok())
                co_return;
            target = at.point;
        }
        PasteOptions opts;
        opts.target = target;
        opts.asBlock = mode_ == Mode::Block;
        auto ids = paste(ctx.tx(), clip, opts);
        if (!ids) {
            ctx.message(ids.error().message);
            co_return;
        }
        ctx.host().setSelection(ids.value());
        ctx.message(plural(ids.value().size()) + " pasted.");
    }

private:
    std::string name_;
    Mode mode_;
};

// WBLOCK [W]: writes the selected objects, with the layers, styles and blocks they use, to a
// drawing file whose origin is the chosen base point.
class WBlockCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "WBLOCK"; }
    std::vector<std::string> defaultAliases() const override { return {"W"}; }
    std::string_view description() const override {
        return "Write the selected objects to a drawing file";
    }

    cmd::Task run(cmd::CommandContext& ctx) override {
        cmd::SelectionOptions so;
        so.filter = [&doc = ctx.doc()](const Entity& e) { return visibleEntity(doc, e); };
        auto sel = co_await ctx.getSelection("Select objects to write", so);
        if (!sel.ok())
            co_return;
        auto b = co_await ctx.getPoint("Specify base point");
        if (!b.ok())
            co_return;
        auto f = co_await ctx.getString("Enter the file name <block.tbcad>",
                                        cmd::StringOptions{.defaultValue = "block.tbcad"});
        if (!f.ok())
            co_return;
        std::string file = cmd::trim(f.text);
        if (std::filesystem::path(file).extension().empty())
            file += ".tbcad";

        Clip clip = capture(ctx.doc(), sel.ids, b.point);
        if (clip.empty()) {
            ctx.message("Nothing to write.");
            co_return;
        }
        Document out;
        {
            Transaction tx = out.begin("WBLOCK");
            PasteOptions opts;
            opts.target = {0, 0};
            auto ids = paste(tx, clip, opts);
            if (!ids) {
                ctx.message(ids.error().message);
                co_return;
            }
            tx.commit();
        }
        auto r = io::writeTbcad(out, std::filesystem::path(std::u8string(file.begin(), file.end())),
                                io::WriteOptions{.keepBackup = false});
        if (!r) {
            ctx.message("Cannot write " + file + ": " + r.error().message);
            co_return;
        }
        ctx.message(plural(clip.entities.size()) + " written to " + file + ".");
    }
};

}  // namespace

void registerCommands(cmd::Registry& registry) {
    (void)registry.add(std::make_unique<CopyCommand>("COPYCLIP", false, false));
    (void)registry.add(std::make_unique<CopyCommand>("COPYBASE", true, false));
    (void)registry.add(std::make_unique<CopyCommand>("CUTCLIP", false, true));
    (void)registry.add(std::make_unique<PasteCommand>("PASTECLIP", PasteCommand::Mode::Normal));
    (void)registry.add(std::make_unique<PasteCommand>("PASTEBLOCK", PasteCommand::Mode::Block));
    (void)registry.add(std::make_unique<PasteCommand>("PASTEORIG", PasteCommand::Mode::Original));
    (void)registry.add(std::make_unique<WBlockCommand>());
}

void registerUi(ui::UiRegistry& ui) {
    ui.addMenuItem("Edit/Cut", "CUTCLIP", "edit.cut", "Ctrl+X");
    ui.addMenuItem("Edit/Copy", "COPYCLIP", "edit.copy", "Ctrl+C");
    ui.addMenuItem("Edit/Copy with Base Point", "COPYBASE", "edit.copybase", "Ctrl+Shift+C");
    ui.addMenuItem("Edit/Paste", "PASTECLIP", "edit.paste", "Ctrl+V");
    ui.addMenuItem("Edit/Paste as Block", "PASTEBLOCK", "edit.pasteblock", "Ctrl+Shift+V");
    ui.addMenuItem("Edit/Paste to Original Coordinates", "PASTEORIG", "edit.pasteorig", "");
}

}  // namespace tbcad::clipboard
