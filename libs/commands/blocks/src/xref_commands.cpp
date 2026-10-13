#include <filesystem>

#include "commands.h"
#include "tbcad/blocks/tbcad_blocks.h"
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
        case xref::State::NotXref:
            break;
    }
    return "";
}

// XATTACH [XA]: attaches a .tbcad or .dxf file as an external reference and inserts it.
class XAttachCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "XATTACH"; }
    std::vector<std::string> defaultAliases() const override { return {"XA"}; }
    std::string_view description() const override {
        return "Attach a drawing as an external reference";
    }

    cmd::Task run(cmd::CommandContext& ctx) override {
        Document& doc = ctx.doc();
        const std::filesystem::path hostFile = hostFileOf(ctx);

        xref::Source source;
        std::string stored;
        for (;;) {
            auto f = co_await ctx.getString("Enter the .tbcad or .dxf file to attach");
            if (!f.ok())
                co_return;
            stored = cmd::trim(f.text);
            if (stored.size() >= 2 && stored.front() == '"' && stored.back() == '"')
                stored = stored.substr(1, stored.size() - 2);
            auto loaded = xref::loadSource(xref::resolvePath(stored, hostFile));
            if (loaded.ok()) {
                source = std::move(loaded.value());
                break;
            }
            ctx.message(loaded.error().message);
        }

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
        auto attached = xref::attach(ctx.tx(), doc, blockName, stored, source, &sum);
        if (!attached) {
            ctx.message(attached.error().message);
            co_return;
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
                auto s = co_await ctx.getReal("Specify scale factor <1>",
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
        auto added = ctx.tx().add(std::move(e));
        if (!added) {
            ctx.message(added.error().message);
            co_return;
        }
        ctx.host().setSelection({added.value()});
        ctx.setUndoLabel("Attach " + blockName);
        ctx.message("Attached \"" + blockName + "\": " + std::to_string(sum.entities) +
                    " objects, " + std::to_string(sum.layers) + " layers");
    }
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
            auto loaded = xref::loadSource(xref::resolvePath(doc.block(id)->xrefPath, hostFile));
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
    enum class Mode { Detach, Bind };
    explicit XrefByNameCommand(Mode mode) : mode_(mode) {}

    std::string_view name() const override { return mode_ == Mode::Detach ? "XDETACH" : "XBIND"; }
    std::vector<std::string> defaultAliases() const override {
        return mode_ == Mode::Bind ? std::vector<std::string>{"XB"} : std::vector<std::string>{};
    }
    std::string_view description() const override {
        return mode_ == Mode::Detach ? "Remove an external reference"
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
                co_await ctx.getString(mode_ == Mode::Detach ? "Enter xref name to detach or [?]"
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
                        stateText(xref::state(doc, id, hostFile)) + ", " +
                        std::to_string(xref::useCount(doc, id)) + " inserted]");
        }
    }
};

}  // namespace

std::unique_ptr<cmd::Command> makeXAttachCommand() {
    return std::make_unique<XAttachCommand>();
}
std::unique_ptr<cmd::Command> makeXReloadCommand() {
    return std::make_unique<XReloadCommand>();
}
std::unique_ptr<cmd::Command> makeXDetachCommand() {
    return std::make_unique<XrefByNameCommand>(XrefByNameCommand::Mode::Detach);
}
std::unique_ptr<cmd::Command> makeXBindCommand() {
    return std::make_unique<XrefByNameCommand>(XrefByNameCommand::Mode::Bind);
}
std::unique_ptr<cmd::Command> makeXListCommand() {
    return std::make_unique<XListCommand>();
}

}  // namespace tbcad::block_cmds
