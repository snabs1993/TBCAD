#include <algorithm>
#include <cmath>
#include <filesystem>

#include "commands.h"
#include "tbcad/blocks/tbcad_blocks.h"
#include "tbcad/underlay/image.h"
#include "tbcad/underlay/pdf.h"
#include "tbcad/xref/xref.h"

namespace tbcad::block_cmds {

namespace {

constexpr const char* kHostFileSetting = "document.file";

std::filesystem::path hostFileOf(cmd::CommandContext& ctx) {
    auto f = ctx.host().setting(kHostFileSetting);
    return f ? std::filesystem::path(std::u8string(f->begin(), f->end())) : std::filesystem::path{};
}

std::string lower(std::string s) {
    for (char& c : s)
        if (c >= 'A' && c <= 'Z')
            c = static_cast<char>(c - 'A' + 'a');
    return s;
}

std::string xrefNames(const Document& doc) {
    std::string s;
    for (BlockId id : xref::xrefs(doc)) {
        if (!s.empty())
            s += ", ";
        s += doc.block(id)->name;
    }
    return s.empty() ? "(no external references)" : s;
}

const char* stateText(xref::State s) {
    switch (s) {
        case xref::State::UpToDate:
            return "up to date";
        case xref::State::Changed:
            return "changed on disk";
        case xref::State::Missing:
            return "file not found";
        case xref::State::Unloaded:
            return "unloaded";
        case xref::State::NotXref:
            break;
    }
    return "";
}

// XATTACH [XA]: attaches a .tbcad, .dxf or .pdf file as an external reference and inserts it.
// PDFATTACH [PA] is the same for a PDF page: it also asks for the page and how much to fade it.
// IMAGEATTACH [IAT] attaches a PNG, JPEG or BMP picture and asks how much to fade it.
class XAttachCommand final : public cmd::Command {
public:
    enum class Kind { Drawing, Pdf, Image };
    explicit XAttachCommand(Kind kind) : kind_(kind), pdf_(kind == Kind::Pdf) {}

    std::string_view name() const override {
        return pdf_ ? "PDFATTACH" : kind_ == Kind::Image ? "IMAGEATTACH" : "XATTACH";
    }
    std::vector<std::string> defaultAliases() const override {
        return pdf_                   ? std::vector<std::string>{"PA"}
               : kind_ == Kind::Image ? std::vector<std::string>{"IAT"}
                                      : std::vector<std::string>{"XA"};
    }
    std::string_view description() const override {
        return pdf_                   ? "Attach a PDF page as an underlay to look at and trace"
               : kind_ == Kind::Image ? "Attach a picture (PNG, JPEG, BMP) as an underlay"
                                      : "Attach a drawing as an external reference";
    }

    cmd::Task run(cmd::CommandContext& ctx) override {
        Document& doc = ctx.doc();
        const std::filesystem::path hostFile = hostFileOf(ctx);

        xref::Source source;
        std::string stored;
        std::filesystem::path resolved;
        underlay::PdfOptions pdfOptions;
        for (;;) {
            auto f = co_await ctx.getString(pdf_ ? "Enter the .pdf file to attach"
                                            : kind_ == Kind::Image
                                                ? "Enter the .png, .jpg or .bmp file to attach"
                                                : "Enter the .tbcad, .dxf or .pdf file to attach");
            if (!f.ok())
                co_return;
            stored = cmd::trim(f.text);
            if (stored.size() >= 2 && stored.front() == '"' && stored.back() == '"')
                stored = stored.substr(1, stored.size() - 2);
            if (pdf_) {
                const underlay::Spec spec = underlay::parseSpec(stored);
                auto pages = underlay::pdfPageCount(xref::resolvePath(stored, hostFile));
                if (!pages) {
                    ctx.message(pages.error().message);
                    continue;
                }
                underlay::PdfOptions options = spec.pdf;
                if (!spec.hasSpec) {
                    if (pages.value() > 1) {
                        auto pg = co_await ctx.getInteger(
                            "Enter the page number (1-" + std::to_string(pages.value()) + ") <1>",
                            cmd::IntOptions{.defaultValue = 1});
                        if (!pg.ok())
                            co_return;
                        options.page =
                            static_cast<int>(std::clamp<long long>(pg.value, 1, pages.value()));
                    }
                    auto fd = co_await ctx.getReal("Enter the fade, 0 to 90 percent <50>",
                                                   cmd::RealOptions{.defaultValue = 50});
                    if (!fd.ok())
                        co_return;
                    options.fadePercent = std::clamp(fd.value, 0.0, 90.0);
                }
                pdfOptions = options;
                stored = underlay::makeSpec(spec.file, options);
            }
            if (kind_ == Kind::Image && !underlay::isImagePath(stored)) {
                ctx.message("Pictures must be .png, .jpg, .jpeg or .bmp files");
                continue;
            }
            resolved = xref::resolvePath(stored, hostFile);
            if (!pdf_ && underlay::isPdfPath(stored))
                pdfOptions = underlay::parseSpec(stored).pdf;
            auto loaded = xref::loadStored(stored, hostFile);
            if (loaded.ok()) {
                source = std::move(loaded.value());
                break;
            }
            ctx.message(loaded.error().message);
        }
        double imageFade = 0;
        if (kind_ == Kind::Image) {
            auto fd =
                co_await ctx.getReal("Enter the fade, 0 to 95 percent <30>",
                                     cmd::RealOptions{.defaultValue = underlay::kDefaultImageFade});
            if (!fd.ok())
                co_return;
            imageFade = std::clamp(fd.value, 0.0, underlay::kMaxImageFade);
        }

        const bool underlayKind = pdf_ || kind_ == Kind::Image;
        bool overlay = false;
        if (!underlayKind) {  // a picture or PDF page is never an overlay
            auto kind = co_await ctx.getKeyword(
                "Reference type [Attach/Overlay] <Attach>",
                cmd::KeywordOptions{.keywords = {"Attach", "Overlay"}, .defaultKeyword = "Attach"});
            if (!kind.ok())
                co_return;
            overlay = kind.keyword == "Overlay";
        }
        auto pathKind = co_await ctx.getKeyword(
            "Path type [Relative/Absolute/None] <Relative>",
            cmd::KeywordOptions{.keywords = {"Relative", "Absolute", "None"},
                                .defaultKeyword = "Relative"});
        if (!pathKind.ok())
            co_return;
        xref::PathType pathType = pathKind.keyword == "Absolute" ? xref::PathType::Absolute
                                  : pathKind.keyword == "None"   ? xref::PathType::None
                                                                 : xref::PathType::Relative;
        if (pathType == xref::PathType::Relative && hostFile.empty())
            ctx.message("The drawing is not saved yet: the full path is stored.");
        stored = xref::storedPath(resolved, hostFile, pathType);
        if (pdf_ || underlay::isPdfPath(resolved.string()))
            stored = underlay::makeSpec(stored, pdfOptions);

        std::string blockName;
        const std::string suggested = xref::defaultName(xref::resolvePath(stored, hostFile));
        for (;;) {
            cmd::StringOptions so;
            so.defaultValue = suggested;
            auto n =
                co_await ctx.getString("Enter the xref name <" + suggested + ">", std::move(so));
            if (!n.ok())
                co_return;
            blockName = cmd::trim(n.text);
            auto ok = blocks::validateBlockName(doc, blockName);
            if (ok.ok())
                break;
            ctx.message(ok.error().message);
        }

        // Attach first so the preview can show the real content; Esc rolls it back.
        xref::Summary sum;
        auto attached = xref::attach(ctx.tx(), doc, blockName, stored, source, &sum, overlay);
        if (!attached) {
            ctx.message(attached.error().message);
            co_return;
        }
        if (kind_ == Kind::Image && imageFade != 0) {
            underlay::ImageParams params;
            params.fade = imageFade;
            auto set = underlay::setImageParams(ctx.tx(), blockName, params);
            if (!set) {
                ctx.message(set.error().message);
                co_return;
            }
        }
        InsertData data{attached.value(), {}, {1, 1}, 0};
        for (;;) {
            cmd::PointOptions po;
            po.keywords = {"Scale", "Rotate"};
            po.preview = [&doc, &ctx, data](Vec2 cursor, DisplayList& out) {
                InsertData d = data;
                d.position = cursor;
                Entity e;
                e.common.layer = doc.header().currentLayer;
                e.data = Indirect<InsertData>(d);
                geom::DisplayContext dc;
                dc.pixelSizeMm = ctx.host().view().scale;
                blocks::buildInsertDisplay(doc, e, dc, out);
            };
            auto p = co_await ctx.getPoint("Specify insertion point <0,0> or [Scale/Rotate]",
                                           std::move(po));
            if (p.status == cmd::Status::Cancel)
                co_return;
            if (p.ok()) {
                data.position = p.point;
                break;
            }
            if (p.status == cmd::Status::None)
                break;  // Enter: insert at the origin
            if (p.keyword == "Scale") {
                auto s = co_await ctx.getReal(kind_ == Kind::Image
                                                  ? "Specify the size of one pixel in mm <1>"
                                                  : "Specify scale factor <1>",
                                              cmd::RealOptions{.defaultValue = 1});
                if (!s.ok())
                    co_return;
                if (s.value == 0) {
                    ctx.message("A scale factor cannot be zero");
                    continue;
                }
                data.scale = {s.value, s.value};
            } else if (p.keyword == "Rotate") {
                auto a = co_await ctx.getAngle("Specify rotation angle <0>",
                                               cmd::AngleOptions{.defaultValue = 0.0});
                if (!a.ok())
                    co_return;
                data.rotation = normalizeAngle(a.value);
            }
        }
        Entity e;
        e.data = Indirect<InsertData>(data);
        applyCurrentProperties(ctx.doc().header(), e);
        auto added = ctx.tx().add(std::move(e));
        if (!added) {
            ctx.message(added.error().message);
            co_return;
        }
        ctx.host().setSelection({added.value()});
        ctx.setUndoLabel("Attach " + blockName);
        ctx.message((overlay ? "Overlaid \"" : "Attached \"") + blockName +
                    "\": " + std::to_string(sum.entities) + " objects, " +
                    std::to_string(sum.layers) + " layers");
    }

private:
    Kind kind_;
    bool pdf_;
};

// XRELOAD [XR]: reloads external references from their files.
class XReloadCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "XRELOAD"; }
    std::vector<std::string> defaultAliases() const override { return {"XR"}; }
    std::string_view description() const override { return "Reload external references"; }

    cmd::Task run(cmd::CommandContext& ctx) override {
        Document& doc = ctx.doc();
        if (xref::xrefs(doc).empty()) {
            ctx.message("There are no external references in this drawing.");
            co_return;
        }
        auto n = co_await ctx.getString("Enter xref name or [All] <All>",
                                        cmd::StringOptions{.defaultValue = "All"});
        if (!n.ok())
            co_return;
        std::vector<BlockId> targets;
        const std::string text = cmd::trim(n.text);
        if (text.empty() || lower(text) == "all") {
            targets = xref::xrefs(doc);
        } else {
            const BlockDef* def = doc.blockByName(text);
            if (!def || def->xrefPath.empty()) {
                ctx.message("No external reference named \"" + text +
                            "\". Xrefs: " + xrefNames(doc));
                co_return;
            }
            targets.push_back(def->id);
        }
        const std::filesystem::path hostFile = hostFileOf(ctx);
        std::size_t done = 0;
        for (BlockId id : targets) {
            const std::string blockName = doc.block(id)->name;
            auto loaded = xref::loadStored(doc.block(id)->xrefPath, hostFile);
            if (!loaded) {
                ctx.message("\"" + blockName + "\": " + loaded.error().message);
                continue;
            }
            xref::Summary sum;
            auto r = xref::reload(ctx.tx(), doc, id, loaded.value(), &sum);
            if (!r) {
                ctx.message("\"" + blockName + "\": " + r.error().message);
                continue;
            }
            ctx.message("Reloaded \"" + blockName + "\": " + std::to_string(sum.entities) +
                        " objects");
            ++done;
        }
        if (done == 0)
            co_return;
        ctx.setUndoLabel("Reload xref");
    }
};

// XDETACH and XBIND: one xref chosen by name.
class XrefByNameCommand final : public cmd::Command {
public:
    enum class Mode { Detach, Bind, Unload };
    explicit XrefByNameCommand(Mode mode) : mode_(mode) {}

    std::string_view name() const override {
        return mode_ == Mode::Detach ? "XDETACH" : mode_ == Mode::Unload ? "XUNLOAD" : "XBIND";
    }
    std::vector<std::string> defaultAliases() const override {
        return mode_ == Mode::Bind ? std::vector<std::string>{"XB"} : std::vector<std::string>{};
    }
    std::string_view description() const override {
        return mode_ == Mode::Detach   ? "Remove an external reference"
               : mode_ == Mode::Unload ? "Unload an external reference (keeps its place)"
                                       : "Make an external reference a part of the drawing";
    }

    cmd::Task run(cmd::CommandContext& ctx) override {
        Document& doc = ctx.doc();
        if (xref::xrefs(doc).empty()) {
            ctx.message("There are no external references in this drawing.");
            co_return;
        }
        BlockId id{};
        for (;;) {
            auto n =
                co_await ctx.getString(mode_ == Mode::Detach   ? "Enter xref name to detach or [?]"
                                       : mode_ == Mode::Unload ? "Enter xref name to unload or [?]"
                                                               : "Enter xref name to bind or [?]");
            if (!n.ok())
                co_return;
            const std::string text = cmd::trim(n.text);
            if (text == "?") {
                ctx.message("Xrefs: " + xrefNames(doc));
                continue;
            }
            const BlockDef* def = doc.blockByName(text);
            if (def && !def->xrefPath.empty()) {
                id = def->id;
                break;
            }
            ctx.message("No external reference named \"" + text + "\". Xrefs: " + xrefNames(doc));
        }
        const std::string blockName = doc.block(id)->name;
        if (mode_ == Mode::Detach) {
            auto r = xref::detach(ctx.tx(), doc, id);
            if (!r) {
                ctx.message(r.error().message);
                co_return;
            }
            ctx.host().setSelection({});
            ctx.setUndoLabel("Detach " + blockName);
            ctx.message("Detached \"" + blockName + "\"");
        } else if (mode_ == Mode::Unload) {
            auto r = xref::unload(ctx.tx(), doc, id);
            if (!r) {
                ctx.message(r.error().message);
                co_return;
            }
            ctx.host().setSelection({});
            ctx.setUndoLabel("Unload " + blockName);
            ctx.message("Unloaded \"" + blockName + "\"; XRELOAD brings it back");
        } else {
            auto r = xref::bind(ctx.tx(), doc, id);
            if (!r) {
                ctx.message(r.error().message);
                co_return;
            }
            ctx.setUndoLabel("Bind " + blockName);
            ctx.message("\"" + blockName + "\" is now an ordinary block; its layers stay under " +
                        blockName);
        }
    }

private:
    Mode mode_;
};

class XListCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "XLIST"; }
    std::vector<std::string> defaultAliases() const override { return {"XREF"}; }
    std::string_view description() const override { return "List external references"; }

    cmd::Task run(cmd::CommandContext& ctx) override {
        Document& doc = ctx.doc();
        auto ids = xref::xrefs(doc);
        if (ids.empty()) {
            ctx.message("There are no external references in this drawing.");
            co_return;
        }
        const std::filesystem::path hostFile = hostFileOf(ctx);
        for (BlockId id : ids) {
            const BlockDef* def = doc.block(id);
            ctx.message(def->name + "  " + def->xrefPath + "  [" +
                        (def->xrefOverlay ? "overlay, " : "") +
                        stateText(xref::state(doc, id, hostFile)) + ", " +
                        std::to_string(xref::useCount(doc, id)) + " inserted]");
        }
    }
};

// Names of the underlay blocks (pictures and PDF pages), for the prompts.
std::vector<const BlockDef*> underlayBlocks(const Document& doc, bool imagesOnly) {
    std::vector<const BlockDef*> out;
    for (BlockId id : xref::xrefs(doc)) {
        const BlockDef* def = doc.block(id);
        if (!def)
            continue;
        if (underlay::isImagePath(def->xrefPath) ||
            (!imagesOnly && underlay::isPdfPath(def->xrefPath)))
            out.push_back(def);
    }
    return out;
}

std::string namesOf(const std::vector<const BlockDef*>& defs) {
    std::string s;
    for (const BlockDef* d : defs)
        s += (s.empty() ? "" : ", ") + d->name;
    return s;
}

// Asks for the name of one underlay; the only one is taken without asking.
cmd::Task pickUnderlay(cmd::CommandContext& ctx, bool imagesOnly, const BlockDef*& out,
                       const char* prompt) {
    const Document& doc = ctx.doc();
    const auto defs = underlayBlocks(doc, imagesOnly);
    if (defs.empty()) {
        ctx.message(imagesOnly ? "There are no pictures attached. Use IMAGEATTACH first."
                               : "There are no underlays attached. Use IMAGEATTACH or PDFATTACH.");
        co_return;
    }
    if (defs.size() == 1) {
        out = defs[0];
        co_return;
    }
    for (;;) {
        auto n = co_await ctx.getString(std::string(prompt) + " [" + namesOf(defs) + "]");
        if (!n.ok())
            co_return;
        const std::string text = cmd::trim(n.text);
        for (const BlockDef* d : defs)
            if (lower(d->name) == lower(text)) {
                out = d;
                co_return;
            }
        ctx.message("No underlay named \"" + text + "\". Underlays: " + namesOf(defs));
    }
}

// IMAGECLIP [ICLIP]: shows only a rectangle of a picture. Two corners; Delete shows it all again.
class ImageClipCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "IMAGECLIP"; }
    std::vector<std::string> defaultAliases() const override { return {"ICLIP"}; }
    std::string_view description() const override { return "Show only a rectangle of a picture"; }

    cmd::Task run(cmd::CommandContext& ctx) override {
        Document& doc = ctx.doc();
        const BlockDef* def = nullptr;
        co_await pickUnderlay(ctx, true, def, "Enter the picture to clip");
        if (!def)
            co_return;
        const std::string blockName = def->name;
        underlay::ImageParams params = underlay::imageParams(doc.header(), blockName);

        // The first insert of the block places the picture.
        const InsertData* ins = nullptr;
        for (EntityId id : doc.modelSpace()) {
            const Entity* e = doc.find(id);
            const InsertData* d = e ? entityAs<InsertData>(*e) : nullptr;
            if (d && d->block == def->id) {
                ins = d;
                break;
            }
        }
        if (!ins) {
            ctx.message("\"" + blockName + "\" is not inserted in the drawing.");
            co_return;
        }
        auto info = underlay::imageInfo(xref::resolvePath(def->xrefPath, hostFileOf(ctx)));
        if (!info) {
            ctx.message(info.error().message);
            co_return;
        }
        auto p1 = co_await ctx.getPoint("Specify the first corner of the clip or [Delete]",
                                        cmd::PointOptions{.keywords = {"Delete"}});
        if (!p1.ok() && p1.keyword != "Delete")
            co_return;
        if (p1.keyword == "Delete") {
            params.u0 = params.v0 = 0;
            params.u1 = params.v1 = 1;
        } else {
            cmd::PointOptions po;
            po.base = p1.point;
            auto p2 = co_await ctx.getPoint("Specify the opposite corner", std::move(po));
            if (!p2.ok())
                co_return;
            // World point to picture fractions (u from the left, v from the top).
            const double c = std::cos(-ins->rotation), s = std::sin(-ins->rotation);
            auto toUv = [&](Vec2 w) {
                const double dx = w.x - ins->position.x, dy = w.y - ins->position.y;
                const double lx = (c * dx - s * dy) / ins->scale.x + def->base.x;
                const double ly = (s * dx + c * dy) / ins->scale.y + def->base.y;
                return Vec2{std::clamp(lx / info.value().width, 0.0, 1.0),
                            std::clamp(1.0 - ly / info.value().height, 0.0, 1.0)};
            };
            const Vec2 a = toUv(p1.point), b = toUv(p2.point);
            params.u0 = std::min(a.x, b.x);
            params.u1 = std::max(a.x, b.x);
            params.v0 = std::min(a.y, b.y);
            params.v1 = std::max(a.y, b.y);
            if (params.u1 - params.u0 < 1e-4 || params.v1 - params.v0 < 1e-4) {
                ctx.message("The clip rectangle does not cover the picture.");
                co_return;
            }
        }
        auto set = underlay::setImageParams(ctx.tx(), blockName, params);
        if (!set) {
            ctx.message(set.error().message);
            co_return;
        }
        ctx.setUndoLabel("Clip " + blockName);
        ctx.message(params.clipped() ? "Clipped \"" + blockName + "\""
                                     : "\"" + blockName + "\" is shown in full");
    }
};

// UNDERLAYFADE [UF]: how faded a picture or PDF page is shown. 0 = as is, 95 = nearly gone.
class UnderlayFadeCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "UNDERLAYFADE"; }
    std::vector<std::string> defaultAliases() const override { return {"UF"}; }
    std::string_view description() const override { return "Change how faded an underlay is"; }

    cmd::Task run(cmd::CommandContext& ctx) override {
        Document& doc = ctx.doc();
        const BlockDef* def = nullptr;
        co_await pickUnderlay(ctx, false, def, "Enter the underlay");
        if (!def)
            co_return;
        const std::string blockName = def->name;
        const bool isImage = underlay::isImagePath(def->xrefPath);
        const double current = isImage ? underlay::imageParams(doc.header(), blockName).fade
                                       : underlay::parseSpec(def->xrefPath).pdf.fadePercent;
        auto fd = co_await ctx.getReal(
            "Enter the fade, 0 to 95 percent <" + std::to_string(static_cast<int>(current)) + ">",
            cmd::RealOptions{.defaultValue = current});
        if (!fd.ok())
            co_return;
        const double fade = std::clamp(fd.value, 0.0, underlay::kMaxImageFade);
        if (isImage) {
            underlay::ImageParams params = underlay::imageParams(doc.header(), blockName);
            params.fade = fade;
            auto set = underlay::setImageParams(ctx.tx(), blockName, params);
            if (!set) {
                ctx.message(set.error().message);
                co_return;
            }
        } else {
            // A PDF page is drawn in faded colours: rewrite the stored path and read the page
            // again.
            underlay::Spec spec = underlay::parseSpec(def->xrefPath);
            spec.pdf.fadePercent = std::min(fade, 90.0);
            const std::string stored = underlay::makeSpec(spec.file, spec.pdf);
            auto loaded = xref::loadStored(stored, hostFileOf(ctx));
            if (!loaded) {
                ctx.message(loaded.error().message);
                co_return;
            }
            const BlockId id = def->id;
            auto mod = ctx.tx().modifyBlock(id, [&](BlockDef& b) { b.xrefPath = stored; });
            if (!mod) {
                ctx.message(mod.error().message);
                co_return;
            }
            auto r = xref::reload(ctx.tx(), doc, id, loaded.value());
            if (!r) {
                ctx.message(r.error().message);
                co_return;
            }
        }
        ctx.setUndoLabel("Fade " + blockName);
        ctx.message("\"" + blockName + "\" fade is now " + std::to_string(static_cast<int>(fade)) +
                    "%");
    }
};

}  // namespace

std::unique_ptr<cmd::Command> makeXAttachCommand() {
    return std::make_unique<XAttachCommand>(XAttachCommand::Kind::Drawing);
}

std::unique_ptr<cmd::Command> makePdfAttachCommand() {
    return std::make_unique<XAttachCommand>(XAttachCommand::Kind::Pdf);
}
std::unique_ptr<cmd::Command> makeImageAttachCommand() {
    return std::make_unique<XAttachCommand>(XAttachCommand::Kind::Image);
}
std::unique_ptr<cmd::Command> makeImageClipCommand() {
    return std::make_unique<ImageClipCommand>();
}
std::unique_ptr<cmd::Command> makeUnderlayFadeCommand() {
    return std::make_unique<UnderlayFadeCommand>();
}
std::unique_ptr<cmd::Command> makeXReloadCommand() {
    return std::make_unique<XReloadCommand>();
}
std::unique_ptr<cmd::Command> makeXDetachCommand() {
    return std::make_unique<XrefByNameCommand>(XrefByNameCommand::Mode::Detach);
}
std::unique_ptr<cmd::Command> makeXUnloadCommand() {
    return std::make_unique<XrefByNameCommand>(XrefByNameCommand::Mode::Unload);
}
std::unique_ptr<cmd::Command> makeXBindCommand() {
    return std::make_unique<XrefByNameCommand>(XrefByNameCommand::Mode::Bind);
}
std::unique_ptr<cmd::Command> makeXListCommand() {
    return std::make_unique<XListCommand>();
}

}  // namespace tbcad::block_cmds
