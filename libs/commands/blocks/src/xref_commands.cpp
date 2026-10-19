#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>

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
            auto f = co_await ctx.getString(
                std::string(pdf_ ? "Enter the .pdf file to attach"
                            : kind_ == Kind::Image
                                ? "Enter the .png, .jpg or .bmp file to attach"
                                : "Enter the .tbcad, .dxf, .dwg or .pdf file to attach") +
                (ctx.host().dialogsAvailable() ? " (? to browse)" : ""));
            if (!f.ok())
                co_return;
            if (cmd::trim(f.text) == "?" && ctx.host().dialogsAvailable()) {
                // The window shows a file dialog and types the chosen path as the next answer.
                ctx.host().requestUi(std::string("pickfile:") + (pdf_ ? "pdf"
                                                                 : kind_ == Kind::Image
                                                                     ? "image"
                                                                     : "drawing"));
                continue;
            }
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

// XRELOAD: reloads external references from their files.
class XReloadCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "XRELOAD"; }
    std::vector<std::string> defaultAliases() const override { return {}; }
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
            auto how = co_await ctx.getKeyword(
                "Bind type [Bind/Insert] <Bind>",
                cmd::KeywordOptions{.keywords = {"Bind", "Insert"}, .defaultKeyword = "Bind"});
            if (!how.ok())
                co_return;
            const bool insert = how.keyword == "Insert";
            auto r = xref::bind(ctx.tx(), doc, id,
                                insert ? xref::BindMode::Insert : xref::BindMode::Bind);
            if (!r) {
                ctx.message(r.error().message);
                co_return;
            }
            ctx.setUndoLabel((insert ? "Insert " : "Bind ") + blockName);
            ctx.message(insert ? "\"" + blockName +
                                     "\" is now an ordinary block; its layers were merged into "
                                     "the drawing's own"
                               : "\"" + blockName +
                                     "\" is now an ordinary block; its layers stay under " +
                                     blockName);
        }
    }

private:
    Mode mode_;
};

// Asks for the name of an xref (listing them on "?"). Leaves `id` null on Esc.
cmd::Task askXrefName(cmd::CommandContext& ctx, const char* prompt, BlockId& id) {
    Document& doc = ctx.doc();
    for (;;) {
        auto n = co_await ctx.getString(prompt);
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
            co_return;
        }
        ctx.message("No external reference named \"" + text + "\". Xrefs: " + xrefNames(doc));
    }
}

// XPATH: changes how the path of an xref is stored (Relative / Absolute / None) without moving
// the file it points to.
class XPathCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "XPATH"; }
    std::string_view description() const override {
        return "Store the path of an external reference as relative, absolute or file name only";
    }
    cmd::Task run(cmd::CommandContext& ctx) override {
        Document& doc = ctx.doc();
        if (xref::xrefs(doc).empty()) {
            ctx.message("There are no external references in this drawing.");
            co_return;
        }
        BlockId id{};
        co_await askXrefName(ctx, "Enter xref name or [?]", id);
        if (!id)
            co_return;
        auto t = co_await ctx.getKeyword(
            "Path type [Relative/Absolute/None] <Relative>",
            cmd::KeywordOptions{.keywords = {"Relative", "Absolute", "None"},
                                .defaultKeyword = "Relative"});
        if (!t.ok())
            co_return;
        const xref::PathType type = t.keyword == "Absolute" ? xref::PathType::Absolute
                                    : t.keyword == "None"   ? xref::PathType::None
                                                            : xref::PathType::Relative;
        const std::filesystem::path hostFile = hostFileOf(ctx);
        const BlockDef* def = doc.block(id);
        const std::string name = def->name;
        const std::filesystem::path file = xref::resolvePath(def->xrefPath, hostFile);
        const std::string stored = xref::storedPath(file, hostFile, type);
        auto r = xref::changePath(ctx.tx(), doc, id, stored);
        if (!r) {
            ctx.message(r.error().message);
            co_return;
        }
        ctx.setUndoLabel("Path of " + name);
        ctx.message("\"" + name + "\" now points to " + stored);
    }
};

// XRELINK: points an xref (usually one whose file was not found) at another file and reloads it.
class XRelinkCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "XRELINK"; }
    std::string_view description() const override {
        return "Point an external reference at another file and reload it";
    }
    cmd::Task run(cmd::CommandContext& ctx) override {
        Document& doc = ctx.doc();
        if (xref::xrefs(doc).empty()) {
            ctx.message("There are no external references in this drawing.");
            co_return;
        }
        BlockId id{};
        co_await askXrefName(ctx, "Enter xref name or [?]", id);
        if (!id)
            co_return;
        auto f = co_await ctx.getString("New file for the reference");
        if (!f.ok() || cmd::trim(f.text).empty())
            co_return;
        const std::filesystem::path hostFile = hostFileOf(ctx);
        const std::string typed = cmd::trim(f.text);
        const std::string name = doc.block(id)->name;
        const std::filesystem::path file = xref::resolvePath(typed, hostFile);
        auto loaded = xref::loadSource(file);
        if (!loaded) {
            ctx.message("Cannot read \"" + typed + "\": " + loaded.error().message);
            co_return;
        }
        const std::string stored = xref::storedPath(file, hostFile, xref::PathType::Relative);
        if (auto r = xref::changePath(ctx.tx(), doc, id, stored); !r) {
            ctx.message(r.error().message);
            co_return;
        }
        xref::Summary sum;
        if (auto r = xref::reload(ctx.tx(), doc, id, loaded.value(), &sum); !r) {
            ctx.message(r.error().message);
            co_return;
        }
        ctx.setUndoLabel("Relink " + name);
        ctx.message("\"" + name + "\" now reads " + stored + ": " + std::to_string(sum.entities) +
                    " objects");
    }
};

class XListCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "XLIST"; }
    std::vector<std::string> defaultAliases() const override { return {}; }
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
            const std::filesystem::path found = xref::resolvePath(def->xrefPath, hostFile);
            const std::string foundText =
                found.string() == def->xrefPath ? "" : "  found at " + found.string();
            ctx.message(def->name + "  " + def->xrefPath + foundText + "  [" +
                        (def->xrefOverlay ? "overlay, " : "") +
                        stateText(xref::state(doc, id, hostFile)) + ", " +
                        std::to_string(xref::useCount(doc, id)) + " inserted]");
        }
    }
};

// ETRANSMIT [ARCHIVE]: copies the saved drawing and everything it refers to (xrefs, pictures,
// PDF underlays) into one folder, keeping relative paths, and writes transmittal.txt. A file
// stored outside the drawing's folder tree goes to "refs" and is listed as needing a new path.
class ETransmitCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "ETRANSMIT"; }
    std::vector<std::string> defaultAliases() const override { return {"ARCHIVE"}; }
    std::string_view description() const override {
        return "Copy the drawing and its references into a folder to send";
    }
    cmd::Task run(cmd::CommandContext& ctx) override {
        namespace fs = std::filesystem;
        const fs::path hostFile = hostFileOf(ctx);
        std::error_code ec;
        if (hostFile.empty() || !fs::exists(hostFile, ec)) {
            ctx.message("Save the drawing first: ETRANSMIT packs the saved file.");
            co_return;
        }
        auto f = co_await ctx.getString("Destination folder");
        if (!f.ok() || cmd::trim(f.text).empty())
            co_return;
        const std::string text = cmd::trim(f.text);
        const fs::path dest(std::u8string(text.begin(), text.end()));
        fs::create_directories(dest, ec);
        if (ec) {
            ctx.message("Cannot create " + text + ": " + ec.message());
            co_return;
        }
        std::string report = "Transmittal of " + hostFile.filename().string() + "\n";
        std::size_t copied = 0, missing = 0, moved = 0;
        auto copy = [&](const fs::path& from, const fs::path& to) {
            std::error_code e;
            fs::create_directories(to.parent_path(), e);
            fs::copy_file(from, to, fs::copy_options::overwrite_existing, e);
            return !e;
        };
        if (copy(hostFile, dest / hostFile.filename())) {
            ++copied;
            report += "drawing  " + hostFile.filename().string() + "\n";
        } else {
            ctx.message("Cannot copy the drawing into " + text + ".");
            co_return;
        }
        Document& doc = ctx.doc();
        for (BlockId id : xref::xrefs(doc)) {
            const BlockDef* def = doc.block(id);
            if (!def)
                continue;
            const fs::path found = xref::resolvePath(def->xrefPath, hostFile);
            if (!fs::exists(found, ec)) {
                ++missing;
                report += "MISSING  " + def->xrefPath + "\n";
                continue;
            }
            const std::string stored = underlay::parseSpec(def->xrefPath).file;
            const fs::path rel(std::u8string(stored.begin(), stored.end()));
            const bool inside = rel.is_relative() && *rel.begin() != "..";
            const fs::path target = inside ? dest / rel : dest / "refs" / found.filename();
            if (!copy(found, target)) {
                ++missing;
                report += "FAILED   " + def->xrefPath + "\n";
                continue;
            }
            ++copied;
            if (inside) {
                report += "xref     " + def->xrefPath + "\n";
            } else {
                ++moved;
                report += "xref     " + def->xrefPath + "  ->  refs/" +
                          found.filename().string() + " (set the path again)\n";
            }
        }
        std::ofstream(dest / "transmittal.txt", std::ios::binary) << report;
        ctx.message(std::to_string(copied) + " file(s) copied to " + text +
                    (missing ? ", " + std::to_string(missing) + " reference(s) not found" : "") +
                    (moved ? ", " + std::to_string(moved) + " moved to refs (new paths needed)"
                           : "") +
                    ". See transmittal.txt.");
    }
};

// EXTERNALREFERENCES [ER]: opens the External References panel.
class XrefPaletteCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "EXTERNALREFERENCES"; }
    std::vector<std::string> defaultAliases() const override { return {"ER", "XREF", "XR"}; }
    std::string_view description() const override { return "Open the External References panel"; }
    cmd::Task run(cmd::CommandContext& ctx) override {
        ctx.host().openPanel("xrefs");
        co_return;
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
        // World point to picture fractions (u from the left, v from the top).
        const double rc = std::cos(-ins->rotation), rs = std::sin(-ins->rotation);
        auto toUv = [&](Vec2 w) {
            const double dx = w.x - ins->position.x, dy = w.y - ins->position.y;
            const double lx = (rc * dx - rs * dy) / ins->scale.x + def->base.x;
            const double ly = (rs * dx + rc * dy) / ins->scale.y + def->base.y;
            return Vec2{std::clamp(lx / info.value().width, 0.0, 1.0),
                        std::clamp(1.0 - ly / info.value().height, 0.0, 1.0)};
        };
        auto p1 =
            co_await ctx.getPoint("Specify the first corner of the clip or [Polygonal/Delete]",
                                  cmd::PointOptions{.keywords = {"Polygonal", "Delete"}});
        if (!p1.ok() && p1.keyword != "Delete" && p1.keyword != "Polygonal")
            co_return;
        if (p1.keyword == "Delete") {
            params.u0 = params.v0 = 0;
            params.u1 = params.v1 = 1;
            params.clip.clear();
        } else if (p1.keyword == "Polygonal") {
            // Corners of the outline, in the world; at least three, closed by Close or Enter.
            std::vector<Vec2> world;
            for (;;) {
                cmd::PointOptions po;
                if (!world.empty()) {
                    po.base = world.back();
                    po.preview = [pts = world](Vec2 c, DisplayList& out) {
                        DisplayPolyline pl;
                        pl.closed = pts.size() >= 2;
                        pl.pts = pts;
                        pl.pts.push_back(c);
                        out.items.emplace_back(std::move(pl));
                    };
                }
                if (world.size() >= 3)
                    po.keywords = {"Close"};
                auto pt =
                    co_await ctx.getPoint(world.empty() ? "Specify the first point of the outline"
                                                        : "Specify the next point of the outline",
                                          std::move(po));
                if (!pt.ok() && pt.keyword != "Close")
                    break;
                if (pt.keyword == "Close")
                    break;
                world.push_back(pt.point);
            }
            if (world.size() < 3) {
                ctx.message("A polygonal clip needs at least three points.");
                co_return;
            }
            params.clip.clear();
            params.u0 = params.v0 = 1;
            params.u1 = params.v1 = 0;
            for (Vec2 w : world) {
                const Vec2 uv = toUv(w);
                params.clip.push_back(uv);
                params.u0 = std::min(params.u0, uv.x);
                params.u1 = std::max(params.u1, uv.x);
                params.v0 = std::min(params.v0, uv.y);
                params.v1 = std::max(params.v1, uv.y);
            }
            if (params.u1 - params.u0 < 1e-4 || params.v1 - params.v0 < 1e-4) {
                ctx.message("The outline does not cover the picture.");
                co_return;
            }
        } else {
            cmd::PointOptions po;
            po.base = p1.point;
            // The rectangle follows the pointer, so the drafter sees what will be kept.
            po.preview = [first = p1.point](Vec2 c, DisplayList& out) {
                DisplayPolyline pl;
                pl.closed = true;
                pl.pts = {first, {c.x, first.y}, c, {first.x, c.y}};
                out.items.emplace_back(std::move(pl));
            };
            auto p2 =
                co_await ctx.getPoint("Specify the opposite corner of the clip", std::move(po));
            if (!p2.ok())
                co_return;
            const Vec2 a = toUv(p1.point), b = toUv(p2.point);
            params.clip.clear();
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
        if (params.clipped()) {
            char kept[96];
            std::snprintf(kept, sizeof kept, "%.0f%% x %.0f%% of the picture is shown",
                          (params.u1 - params.u0) * 100, (params.v1 - params.v0) * 100);
            ctx.message("Clipped \"" + blockName + "\": " + kept +
                        ". The frame still outlines the whole picture.");
        } else {
            ctx.message("\"" + blockName + "\" is shown in full");
        }
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

// IMAGEADJUST [IAD]: brightness and contrast of a picture, 0 to 100 with 50 as the picture is.
class ImageAdjustCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "IMAGEADJUST"; }
    std::vector<std::string> defaultAliases() const override { return {"IAD"}; }
    std::string_view description() const override {
        return "Change the brightness and contrast of a picture";
    }

    cmd::Task run(cmd::CommandContext& ctx) override {
        Document& doc = ctx.doc();
        const BlockDef* def = nullptr;
        co_await pickUnderlay(ctx, false, def, "Enter the picture or PDF to adjust");
        if (!def)
            co_return;
        const std::string blockName = def->name;
        if (!underlay::isImagePath(def->xrefPath)) {
            // A PDF page is drawn in adjusted colours: rewrite the stored path, read it again.
            underlay::Spec spec = underlay::parseSpec(def->xrefPath);
            auto co = co_await ctx.getReal(
                "Enter the contrast, 0 to 100 <" + std::to_string(spec.pdf.contrast) + ">",
                cmd::RealOptions{.defaultValue = spec.pdf.contrast});
            if (!co.ok())
                co_return;
            auto mono = co_await ctx.getKeyword(
                "Monochrome [Yes/No] <" + std::string(spec.pdf.monochrome ? "Yes" : "No") + ">",
                cmd::KeywordOptions{.keywords = {"Yes", "No"},
                                    .defaultKeyword = spec.pdf.monochrome ? "Yes" : "No"});
            if (mono.status == cmd::Status::Cancel)
                co_return;
            spec.pdf.contrast = static_cast<int>(std::lround(std::clamp(co.value, 0.0, 100.0)));
            spec.pdf.monochrome = mono.keyword == "Yes";
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
            ctx.setUndoLabel("Adjust " + blockName);
            ctx.message("\"" + blockName + "\": contrast " + std::to_string(spec.pdf.contrast) +
                        (spec.pdf.monochrome ? ", monochrome" : ""));
            co_return;
        }
        underlay::ImageParams params = underlay::imageParams(doc.header(), blockName);
        auto br = co_await ctx.getReal(
            "Enter the brightness, 0 to 100 <" + std::to_string(params.brightness) + ">",
            cmd::RealOptions{.defaultValue = params.brightness});
        if (!br.ok())
            co_return;
        auto co = co_await ctx.getReal(
            "Enter the contrast, 0 to 100 <" + std::to_string(params.contrast) + ">",
            cmd::RealOptions{.defaultValue = params.contrast});
        if (!co.ok())
            co_return;
        params.brightness = static_cast<int>(std::lround(std::clamp(br.value, 0.0, 100.0)));
        params.contrast = static_cast<int>(std::lround(std::clamp(co.value, 0.0, 100.0)));
        auto mono = co_await ctx.getKeyword(
            "Monochrome [Yes/No] <" + std::string(params.monochrome ? "Yes" : "No") + ">",
            cmd::KeywordOptions{.keywords = {"Yes", "No"},
                                .defaultKeyword = params.monochrome ? "Yes" : "No"});
        if (mono.status == cmd::Status::Cancel)
            co_return;
        params.monochrome = mono.keyword == "Yes";
        auto set = underlay::setImageParams(ctx.tx(), blockName, params);
        if (!set) {
            ctx.message(set.error().message);
            co_return;
        }
        ctx.setUndoLabel("Adjust " + blockName);
        ctx.message("\"" + blockName + "\": brightness " + std::to_string(params.brightness) +
                    ", contrast " + std::to_string(params.contrast));
    }
};

// IMAGEFRAME [IFR]: whether the outline of a picture is drawn (and plotted).
class ImageFrameCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "IMAGEFRAME"; }
    std::vector<std::string> defaultAliases() const override { return {"IFR"}; }
    std::string_view description() const override {
        return "Show, hide or plot the frame of a picture";
    }

    cmd::Task run(cmd::CommandContext& ctx) override {
        Document& doc = ctx.doc();
        const BlockDef* def = nullptr;
        co_await pickUnderlay(ctx, true, def, "Enter the picture");
        if (!def)
            co_return;
        const std::string blockName = def->name;
        underlay::ImageParams params = underlay::imageParams(doc.header(), blockName);
        auto kw =
            co_await ctx.getKeyword("Enter the frame setting [Hide/Show/Plot]",
                                    cmd::KeywordOptions{.keywords = {"Hide", "Show", "Plot"}});
        if (!kw.ok())
            co_return;
        params.frame = kw.keyword == "Hide" ? 0 : kw.keyword == "Plot" ? 1 : 2;
        auto set = underlay::setImageParams(ctx.tx(), blockName, params);
        if (!set) {
            ctx.message(set.error().message);
            co_return;
        }
        ctx.setUndoLabel("Frame " + blockName);
        ctx.message("\"" + blockName + "\" frame: " +
                    std::string(params.frame == 0   ? "hidden"
                                : params.frame == 1 ? "shown and plotted"
                                                    : "shown, not plotted"));
    }
};

// IMAGETRANSPARENCY [ITR]: whether the white background of a black and white picture shows.
class ImageTransparencyCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "IMAGETRANSPARENCY"; }
    std::vector<std::string> defaultAliases() const override { return {"ITR"}; }
    std::string_view description() const override {
        return "Make the background of a black and white picture transparent or opaque";
    }

    cmd::Task run(cmd::CommandContext& ctx) override {
        Document& doc = ctx.doc();
        const BlockDef* def = nullptr;
        co_await pickUnderlay(ctx, true, def, "Enter the picture");
        if (!def)
            co_return;
        const std::string blockName = def->name;
        underlay::ImageParams params = underlay::imageParams(doc.header(), blockName);
        auto kw = co_await ctx.getKeyword("Enter the transparency [ON/OFF]",
                                          cmd::KeywordOptions{.keywords = {"ON", "OFF"}});
        if (!kw.ok())
            co_return;
        params.transparent = kw.keyword == "ON";
        auto set = underlay::setImageParams(ctx.tx(), blockName, params);
        if (!set) {
            ctx.message(set.error().message);
            co_return;
        }
        ctx.setUndoLabel("Transparency " + blockName);
        ctx.message("\"" + blockName + "\" background transparency " +
                    (params.transparent ? "on (black and white pictures only)" : "off"));
    }
};

// PDFOSNAP: 1 lets object snap pick up the lines and corners of PDF underlays (0 = off, default).
class PdfOsnapCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "PDFOSNAP"; }
    std::string_view description() const override {
        return "Snap to the geometry of PDF underlays (0 off, 1 on)";
    }

    cmd::Task run(cmd::CommandContext& ctx) override {
        const int now = pdfOsnap(ctx.doc().header()) ? 1 : 0;
        cmd::IntOptions io;
        io.defaultValue = now;
        io.min = 0;
        io.max = 1;
        auto v = co_await ctx.getInteger(
            "PDFOSNAP 0 ignore PDF underlays, 1 snap to them <" + std::to_string(now) + ">", io);
        if (!v.ok())
            co_return;
        const std::string value = v.value ? "1" : "0";
        (void)ctx.tx().modifyHeader([&](Header& h) { h.custom[kPdfOsnapKey] = value; });
        ctx.setUndoLabel("PDFOSNAP");
        ctx.message("PDFOSNAP = " + value);
    }
};

}  // namespace

std::unique_ptr<cmd::Command> makePdfOsnapCommand() {
    return std::make_unique<PdfOsnapCommand>();
}

std::unique_ptr<cmd::Command> makeImageTransparencyCommand() {
    return std::make_unique<ImageTransparencyCommand>();
}

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
std::unique_ptr<cmd::Command> makeImageAdjustCommand() {
    return std::make_unique<ImageAdjustCommand>();
}
std::unique_ptr<cmd::Command> makeImageFrameCommand() {
    return std::make_unique<ImageFrameCommand>();
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
std::unique_ptr<cmd::Command> makeXPathCommand() {
    return std::make_unique<XPathCommand>();
}
std::unique_ptr<cmd::Command> makeXRelinkCommand() {
    return std::make_unique<XRelinkCommand>();
}
std::unique_ptr<cmd::Command> makeXrefPaletteCommand() {
    return std::make_unique<XrefPaletteCommand>();
}
std::unique_ptr<cmd::Command> makeETransmitCommand() {
    return std::make_unique<ETransmitCommand>();
}
std::unique_ptr<cmd::Command> makeXListCommand() {
    return std::make_unique<XListCommand>();
}

}  // namespace tbcad::block_cmds
