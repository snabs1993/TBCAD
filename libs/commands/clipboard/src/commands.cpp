#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <span>

#include "tbcad/clipboard/clipboard.h"
#include "tbcad/dxf/writer.h"
#include "tbcad/io/tbcad.h"
#include "tbcad/platform/clipboard_picture.h"
#include "tbcad/table/table.h"
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
        // Also for other programs: the text of the selected TEXT / MTEXT (or a marker that
        // Ctrl+V reads as "these objects") and a picture, both put on the system clipboard by the
        // window. Cut does not leave a picture of what is gone.
        std::string text = plainTextOf(ctx.doc(), sel.ids);
        ctx.host().setSetting("clipboard.text", text.empty() ? "[TBCAD objects]" : text);
        // An empty picture still tells the window this text came from an object copy.
        std::optional<platform::ClipboardPicture> pic;
        if (!cut_)
            pic = renderPicture(ctx.doc(), sel.ids);
        platform::stashClipboardPicture(pic ? std::move(*pic) : platform::ClipboardPicture{});
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
    enum class Mode { Normal, Block, Original, Special };
    PasteCommand(std::string name, Mode mode) : name_(std::move(name)), mode_(mode) {}

    std::string_view name() const override { return name_; }
    std::string_view description() const override {
        switch (mode_) {
            case Mode::Block:
                return "Paste the clipboard as one block";
            case Mode::Original:
                return "Paste the clipboard at its original coordinates";
            case Mode::Special:
                return "Paste the clipboard as objects, a block, at the original place or on the "
                       "current layer";
            default:
                return "Paste the clipboard";
        }
    }

    cmd::Task run(cmd::CommandContext& ctx) override {
        const Clip& clip = current();
        Mode mode = mode_;
        bool onCurrentLayer = false;
        if (mode == Mode::Special) {
            cmd::KeywordOptions ko;
            ko.keywords = {"Objects", "Block",     "Original", "CurrentLayer",
                           "Text",    "Hyperlink", "Image",    "Table"};
            ko.defaultKeyword = clip.empty() ? "Text" : "Objects";
            auto how = co_await ctx.getKeyword(
                "Paste as [Objects/Block/Original "
                "coordinates/CurrentLayer/Text/Hyperlink/Image/Table] "
                "<Objects>",
                ko);
            if (!how.ok())
                co_return;
            if (how.keyword == "Image") {
                // The window saves the clipboard picture to a file and starts IMAGEATTACH on it.
                ctx.host().requestUi("pasteimage");
                co_return;
            }
            if (how.keyword == "Table") {
                co_await pasteTable(ctx);
                co_return;
            }
            if (how.keyword == "Text" || how.keyword == "Hyperlink") {
                co_await pasteText(ctx, how.keyword == "Hyperlink");
                co_return;
            }
            if (clip.empty()) {
                ctx.message("The clipboard is empty.");
                co_return;
            }
            mode = how.keyword == "Block"      ? Mode::Block
                   : how.keyword == "Original" ? Mode::Original
                                               : Mode::Normal;
            onCurrentLayer = how.keyword == "CurrentLayer";
        }
        if (clip.empty()) {
            ctx.message("The clipboard is empty.");
            co_return;
        }
        Vec2 target = clip.base;
        if (mode != Mode::Original) {
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
        opts.asBlock = mode == Mode::Block;
        if (onCurrentLayer)
            opts.layer = ctx.doc().header().currentLayer;
        auto ids = paste(ctx.tx(), clip, opts);
        if (!ids) {
            ctx.message(ids.error().message);
            co_return;
        }
        ctx.host().setSelection(ids.value());
        ctx.message(plural(ids.value().size()) + " pasted.");
    }

private:
    // The window answers a clipboardsnapshot request with this (control) text after it has put
    // the system clipboard text in the transient setting below. Nobody can type it.
    static constexpr const char* kSnapshotAnswer = "\x01";
    static constexpr const char* kSnapshotKey = "tmp.clipboard.system";

    // The clipboard text as one MTEXT string: line ends become \P, tabs become spaces.
    static std::string snapshotText(cmd::CommandContext& ctx, bool paragraphs) {
        std::string raw = ctx.host().setting(kSnapshotKey).value_or("");
        std::string out;
        for (std::size_t i = 0; i < raw.size(); ++i) {
            const char c = raw[i];
            if (c == '\r')
                continue;
            if (c == '\n')
                out += paragraphs ? "\\P" : " ";
            else
                out += c == '\t' ? ' ' : c;
        }
        return cmd::trim(out);
    }

    // Cells copied from a spreadsheet (tab separated, or CSV text) become a TABLE.
    static cmd::Task pasteTable(cmd::CommandContext& ctx) {
        ctx.host().requestUi("clipboardsnapshot");
        auto t =
            co_await ctx.getString("Cells to paste (Ctrl+V pastes; copy them from Excel first)");
        if (!t.ok())
            co_return;
        std::string raw =
            t.text == kSnapshotAnswer ? ctx.host().setting(kSnapshotKey).value_or("") : t.text;
        auto rows = table::parseCsv(raw);
        if (rows.empty()) {
            ctx.message("The clipboard holds no cells.");
            co_return;
        }
        auto at = co_await ctx.getPoint("Specify insertion point (top-left corner)");
        if (!at.ok())
            co_return;
        double h = 0;
        const TextStyle* ts = ctx.doc().textStyle(ctx.doc().header().currentTextStyle);
        if (ts && ts->height > 0)
            h = ts->height;
        else if (auto s = ctx.host().setting("text.height"))
            h = std::atof(s->c_str());
        TableData data = table::fromRows(rows, at.point, h > 0 ? h : 2.5, ts);
        data.style = ctx.doc().header().currentTextStyle;
        Entity e;
        e.data = Indirect<TableData>(std::move(data));
        applyCurrentProperties(ctx.doc().header(), e);
        auto id = ctx.tx().add(std::move(e));
        if (!id) {
            ctx.message(id.error().message);
            co_return;
        }
        ctx.host().setSelection({id.value()});
        ctx.message("Table of " + std::to_string(rows.size()) + " rows pasted.");
    }

    // Text (or a text that opens a link): typed or pasted into the command line, since the
    // system clipboard is read by the window, which types it as the answer. A link keeps the
    // address in HYPERLINK form and shows it as the text.
    static cmd::Task pasteText(cmd::CommandContext& ctx, bool link) {
        ctx.host().requestUi("clipboardsnapshot");
        auto t =
            co_await ctx.getString(link ? "Web address or file (Ctrl+V pastes)"
                                        : "Text to paste (Ctrl+V pastes; \\P starts a new line)");
        if (!t.ok())
            co_return;
        std::string text = cmd::trim(t.text);
        if (text == kSnapshotAnswer)  // the window answered with the clipboard text
            text = snapshotText(ctx, !link);
        if (text.empty())
            co_return;
        auto at = co_await ctx.getPoint("Specify insertion point");
        if (!at.ok())
            co_return;
        MTextData d;
        d.position = at.point;
        d.style = ctx.doc().header().currentTextStyle;
        const TextStyle* ts = ctx.doc().textStyle(d.style);
        double h = 0;
        if (ts && ts->height > 0)
            h = ts->height;
        else if (auto s = ctx.host().setting("text.height"))
            h = std::atof(s->c_str());
        d.height = h > 0 ? h : 2.5;
        if (link)
            std::replace(text.begin(), text.end(), '\n', ' ');
        d.utf8 = text;
        Entity created{{}, std::move(d)};
        applyCurrentProperties(ctx.doc().header(), created);
        auto id = ctx.tx().add(std::move(created));
        if (!id) {
            ctx.message(id.error().message);
            co_return;
        }
        if (link) {
            const std::string key = "tbcad.link." + std::to_string(id.value().value);
            (void)ctx.tx().modifyHeader([&](Header& h) { h.custom[key] = text + "\n"; });
        }
        ctx.host().setSelection({id.value()});
        ctx.message(link ? "Hyperlink pasted." : "Text pasted.");
    }

    std::string name_;
    Mode mode_;
};

// WBLOCK [W]: writes the selected objects, with the layers, styles and blocks they use, to a
// drawing file whose origin is the chosen base point. A bare name gets .dxf; a relative name is
// placed next to the open drawing (or the working folder when it was never saved).
class WBlockCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "WBLOCK"; }
    std::vector<std::string> defaultAliases() const override { return {"W"}; }
    std::string_view description() const override {
        return "Write the selected objects to a drawing file";
    }

    static std::filesystem::path pathOf(const std::string& utf8) {
        return std::filesystem::path(std::u8string(utf8.begin(), utf8.end()));
    }
    static std::string utf8Of(const std::filesystem::path& p) {
        const auto u = p.u8string();
        return std::string(u.begin(), u.end());
    }

    static dxf::DxfVersion dxfVersionFor(cmd::CommandContext& ctx) {
        const std::string chosen = ctx.host().setting("files.dxfVersion").value_or("auto");
        if (chosen != "auto")
            if (auto v = dxf::parseVersion(chosen))
                return *v;
        const auto& custom = ctx.doc().header().custom;
        if (auto it = custom.find("dxf.acadver"); it != custom.end() && it->second.size() == 6 &&
                                                  it->second.compare(0, 2, "AC") == 0 &&
                                                  std::atoi(it->second.c_str() + 2) >= 1021)
            return dxf::DxfVersion::R2007;
        return dxf::DxfVersion::R2000;
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
        auto f = co_await ctx.getString("Enter the file name (.dxf or .tbcad) <block.dxf>",
                                        cmd::StringOptions{.defaultValue = "block.dxf"});
        if (!f.ok())
            co_return;
        std::string text = cmd::trim(f.text);
        if (text.size() >= 2 && text.front() == '"' && text.back() == '"')
            text = text.substr(1, text.size() - 2);
        if (text.empty())
            text = "block.dxf";
        std::filesystem::path path = pathOf(text);
        if (path.extension().empty())
            path += ".dxf";
        if (path.is_relative()) {
            const std::string doc = ctx.host().documentFile();
            if (!doc.empty())
                path = pathOf(doc).parent_path() / path;
        }
        std::string ext = utf8Of(path.extension());
        std::transform(ext.begin(), ext.end(), ext.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        const bool asDxf = ext != ".tbcad";
        if (ext != ".dxf" && ext != ".tbcad") {
            ctx.message("Use a .dxf or .tbcad file name.");
            co_return;
        }
        const std::string shown = utf8Of(path);
        if (std::error_code ec; std::filesystem::exists(path, ec)) {
            cmd::KeywordOptions yn;
            yn.keywords = {"Yes", "No"};
            yn.defaultKeyword = "No";
            auto answer =
                co_await ctx.getKeyword(shown + " already exists. Replace it?", std::move(yn));
            if (answer.status != cmd::Status::Keyword || answer.keyword != "Yes") {
                ctx.message("WBLOCK cancelled; the existing file was not changed.");
                co_return;
            }
        }

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
        if (asDxf) {
            dxf::ExportOptions opt;
            opt.version = dxfVersionFor(ctx);
            dxf::Report report;
            auto res = dxf::exportDxf(out, opt, report);
            if (!res) {
                ctx.message("Cannot write " + shown + ": " + res.error().message);
                co_return;
            }
            const auto& bytes = res.value().bytes;
            auto saved = io::saveTbcadBytes(
                std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(bytes.data()),
                                              bytes.size()),
                path, io::WriteOptions{.keepBackup = false});
            if (!saved) {
                ctx.message("Cannot write " + shown + ": " + saved.error().message);
                co_return;
            }
        } else {
            auto r = io::writeTbcad(out, path, io::WriteOptions{.keepBackup = false});
            if (!r) {
                ctx.message("Cannot write " + shown + ": " + r.error().message);
                co_return;
            }
        }
        ctx.message(plural(clip.entities.size()) + " written to " + shown + ".");
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
    (void)registry.add(std::make_unique<PasteCommand>("PASTESPEC", PasteCommand::Mode::Special));
    (void)registry.add(std::make_unique<WBlockCommand>());
}

void registerUi(ui::UiRegistry& ui) {
    ui.addMenuItem("Edit/Cut", "CUTCLIP", "edit.cut", "Ctrl+X");
    ui.addMenuItem("Edit/Copy", "COPYCLIP", "edit.copy", "Ctrl+C");
    ui.addMenuItem("Edit/Copy with Base Point", "COPYBASE", "edit.copybase", "Ctrl+Shift+C");
    ui.addMenuItem("Edit/Paste", "PASTECLIP", "edit.paste", "Ctrl+V");
    ui.addMenuItem("Edit/Paste as Block", "PASTEBLOCK", "edit.pasteblock", "Ctrl+Shift+V");
    ui.addMenuItem("Edit/Paste to Original Coordinates", "PASTEORIG", "edit.pasteorig", "");
    ui.addMenuItem("Edit/Paste Special...", "PASTESPEC", "edit.pastespec", "");
}

}  // namespace tbcad::clipboard
