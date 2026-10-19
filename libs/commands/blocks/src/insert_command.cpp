#include <cmath>
#include <filesystem>

#include "commands.h"
#include "tbcad/block_cmds/palette.h"
#include "tbcad/blocks/explode.h"
#include "tbcad/blocks/tbcad_blocks.h"
#include "tbcad/xref/xref.h"

namespace tbcad::block_cmds {

namespace {

constexpr const char* kLastBlockSetting = "block.lastinsert";
// Folder of drawing files that INSERT accepts by name (the Library tab of the block palette).
constexpr const char* kLibrarySetting = "block.library";
// "0": INSERT does not ask for attribute values and keeps the defaults.
constexpr const char* kAttReqSetting = "block.attreq";

std::string blockNames(const Document& doc) {
    std::string s;
    for (const auto& [id, def] : doc.blocks()) {
        if (def.anonymous)
            continue;
        if (!s.empty())
            s += ", ";
        s += def.name;
    }
    return s.empty() ? "(no blocks defined)" : s;
}

std::filesystem::path pathSetting(cmd::CommandContext& ctx, const char* key) {
    auto v = ctx.host().setting(key);
    return v ? std::filesystem::path(std::u8string(v->begin(), v->end())) : std::filesystem::path{};
}

// A typed name that is not a block but names a drawing file: the file becomes a block (once; an
// existing block of the same name is reused).
const BlockDef* blockFromFile(cmd::CommandContext& ctx, const std::string& typed) {
    Document& doc = ctx.doc();
    std::filesystem::path file =
        libraryFileFor(typed, pathSetting(ctx, "document.file"), pathSetting(ctx, kLibrarySetting));
    if (file.empty())
        return nullptr;
    const std::string name = xref::defaultName(file);
    if (const BlockDef* same = doc.blockByName(name))
        return same;
    auto src = xref::loadSource(file);
    if (!src) {
        ctx.message(src.error().message);
        return nullptr;
    }
    auto id = xref::importAsBlock(ctx.tx(), doc, name, src.value());
    if (!id) {
        ctx.message(id.error().message);
        return nullptr;
    }
    ctx.message("Block \"" + name + "\" made from " + file.filename().string());
    return doc.block(id.value());
}

// INSERT [I]: places an instance of a block with a live preview.
// INSERT and MINSERT. MINSERT places the block as a rows x columns array in one object.
class InsertCommand final : public cmd::Command {
public:
    explicit InsertCommand(bool array = false) : array_(array) {}
    std::string_view name() const override { return array_ ? "MINSERT" : "INSERT"; }
    std::vector<std::string> defaultAliases() const override {
        return array_ ? std::vector<std::string>{} : std::vector<std::string>{"I", "-INSERT"};
    }
    std::string_view description() const override {
        return array_ ? "Insert a block as a rectangular array" : "Insert a block";
    }

    cmd::Task run(cmd::CommandContext& ctx) override {
        Document& doc = ctx.doc();
        // (With no blocks yet a drawing file can still be typed: it becomes the first block.)
        if (doc.blocks().empty())
            ctx.message(
                "There are no blocks in this drawing yet. Type a drawing file name to "
                "insert it, or cancel and use BLOCK to make one.");

        // Block name.
        const BlockDef* def = nullptr;
        for (;;) {
            cmd::StringOptions so;
            if (auto last = ctx.host().setting(kLastBlockSetting);
                last &&
                (doc.blockByName(*last) || !libraryFileFor(*last, pathSetting(ctx, "document.file"),
                                                           pathSetting(ctx, kLibrarySetting))
                                                .empty()))
                so.defaultValue = *last;
            auto n = co_await ctx.getString("Enter block name or file or [?]", std::move(so));
            if (!n.ok())
                co_return;
            std::string text = cmd::trim(n.text);
            if (text == "?") {
                ctx.message("Blocks: " + blockNames(doc));
                continue;
            }
            def = doc.blockByName(text);
            if (!def)
                def = blockFromFile(ctx, text);
            if (def)
                break;
            ctx.message("No block named \"" + text + "\". Blocks: " + blockNames(doc));
        }
        const BlockId block = def->id;
        ctx.host().setSetting(kLastBlockSetting, def->name);

        // Placement. Scale and rotation can be changed before the point is picked.
        InsertData data{block, {}, {1, 1}, ctx.host().ucsAngle()};  // squared to the UCS
        // Keeps placing copies (same scale and rotation) until Enter or Esc, like a drafter
        // dropping furniture symbols one after another.
        bool multiple = false;
        bool explodeAfter = false;  // place the parts, not the block
        for (std::size_t placed = 0;; ++placed) {
            data.attribs = blocks::defaultAttribs(doc, block);
            for (;;) {
                cmd::PointOptions po;
                po.keywords = {"Scale", "X", "Y", "Rotate"};
                if (!array_) {
                    po.keywords.push_back("Multiple");
                    po.keywords.push_back("Explode");
                }
                po.allowNone = placed > 0;  // Enter ends a Multiple series
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
                auto p = co_await ctx.getPoint(
                    placed == 0 ? "Specify insertion point or [Scale/X/Y/Rotate/Multiple/Explode]"
                                : "Specify next insertion point or "
                                  "[Scale/X/Y/Rotate], Enter to finish",
                    std::move(po));
                if (p.status == cmd::Status::Cancel || p.status == cmd::Status::None)
                    co_return;
                if (p.ok()) {
                    data.position = p.point;
                    break;
                }
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
                } else if (p.keyword == "X" || p.keyword == "Y") {
                    auto s = co_await ctx.getReal("Specify " + p.keyword + " scale factor <1>",
                                                  cmd::RealOptions{.defaultValue = 1});
                    if (!s.ok())
                        co_return;
                    if (s.value == 0) {
                        ctx.message("A scale factor cannot be zero");
                        continue;
                    }
                    (p.keyword == "X" ? data.scale.x : data.scale.y) = s.value;
                } else if (p.keyword == "Explode") {
                    explodeAfter = !explodeAfter;
                    ctx.message(explodeAfter ? "The block will be exploded when placed."
                                             : "The block will be placed as one object.");
                } else if (p.keyword == "Multiple") {
                    multiple = true;
                    ctx.commitOnCancel();  // Esc ends the series and keeps the copies already
                                           // placed
                    ctx.message("Placing several copies: Enter or Esc to finish.");
                } else if (p.keyword == "Rotate") {
                    auto a = co_await ctx.getAngle(
                        "Specify rotation angle <0>",
                        cmd::AngleOptions{.defaultValue = 0.0, .inUcs = true});
                    if (!a.ok())
                        co_return;
                    data.rotation = normalizeAngle(a.value);
                }
            }

            if (array_) {
                cmd::IntOptions io;
                io.defaultValue = 1;
                io.min = 1;
                auto rows = co_await ctx.getInteger("Enter number of rows <1>", io);
                if (!rows.ok())
                    co_return;
                auto cols = co_await ctx.getInteger("Enter number of columns <1>", io);
                if (!cols.ok())
                    co_return;
                if (static_cast<long long>(rows.value) * cols.value > 1000000) {
                    ctx.message("That is more than a million copies.");
                    co_return;
                }
                ArrayInfo info;
                info.rows = static_cast<int>(rows.value);
                info.columns = static_cast<int>(cols.value);
                cmd::DistanceOptions dopt;
                dopt.defaultValue = 0.0;
                dopt.allowNegative = true;
                if (rows.value > 1) {
                    auto d = co_await ctx.getDistance("Enter distance between rows", dopt);
                    if (!d.ok())
                        co_return;
                    info.rowSpacing = d.value;
                }
                if (cols.value > 1) {
                    auto d = co_await ctx.getDistance("Enter distance between columns", dopt);
                    if (!d.ok())
                        co_return;
                    info.columnSpacing = d.value;
                }
                info.angle = 0;
                if (rows.value > 1 || cols.value > 1)
                    data.array = info;
            }

            // Attribute values. Preset and constant attributes are not asked for.
            if (ctx.host().setting(kAttReqSetting).value_or("1") != "0" &&
                ctx.host().dialogsAvailable()) {
                // One small dialog for all the values instead of a prompt for each.
                std::string fields;
                for (const Entity* ad : blocks::attributeDefinitions(doc, block)) {
                    const AttDefInfo& info = *entityAs<TextData>(*ad)->attdef;
                    if (info.constant || info.preset)
                        continue;
                    for (const AttribValue& v : data.attribs)
                        if (v.tag == info.tag)
                            fields += v.tag + "\x1f" +
                                      (info.prompt.empty() ? info.tag : info.prompt) + "\x1f" +
                                      v.value + "\x1e";
                }
                if (!fields.empty()) {
                    ctx.host().setSetting("tmp.insatt.block", def->name);
                    ctx.host().setSetting("tmp.insatt.fields", fields);
                    ctx.host().setSetting("tmp.insatt.result", "");
                    ctx.host().openPanel("attvalues");
                    auto r = co_await ctx.getString(
                        "Attribute values: fill in the panel and press OK, or Enter for the "
                        "defaults",
                        cmd::StringOptions{.allowEmpty = true});
                    ctx.host().closePanel("attvalues");
                    if (!r.ok())
                        co_return;
                    const std::string result = ctx.host().setting("tmp.insatt.result").value_or("");
                    std::size_t at = 0;
                    while (at < result.size()) {
                        const std::size_t end = result.find('\x1e', at);
                        const std::string rec = result.substr(at, end - at);
                        const std::size_t sep = rec.find('\x1f');
                        if (sep != std::string::npos)
                            for (AttribValue& v : data.attribs)
                                if (v.tag == rec.substr(0, sep))
                                    v.value = rec.substr(sep + 1);
                        if (end == std::string::npos)
                            break;
                        at = end + 1;
                    }
                }
            } else if (ctx.host().setting(kAttReqSetting).value_or("1") != "0") {
                for (const Entity* ad : blocks::attributeDefinitions(doc, block)) {
                    const AttDefInfo& info = *entityAs<TextData>(*ad)->attdef;
                    if (info.constant || info.preset)
                        continue;
                    for (AttribValue& v : data.attribs) {
                        if (v.tag != info.tag)
                            continue;
                        const std::string label = info.prompt.empty() ? info.tag : info.prompt;
                        auto r = co_await ctx.getString(
                            "Enter attribute value for " + label + " <" + v.value + ">",
                            cmd::StringOptions{.defaultValue = v.value, .allowEmpty = true});
                        if (!r.ok())
                            co_return;
                        v.value = r.text;
                        if (info.verify) {
                            auto again = co_await ctx.getString(
                                "Verify attribute value for " + label + " <" + v.value + ">",
                                cmd::StringOptions{.defaultValue = v.value, .allowEmpty = true});
                            if (!again.ok())
                                co_return;
                            v.value = again.text;
                        }
                    }
                }
            }

            Entity e;
            e.data = Indirect<InsertData>(data);
            applyCurrentProperties(ctx.doc().header(), e);
            // Symbols drawn at 1:1 are sized for the current annotation scale when the drawing
            // asks.
            if (annotateNewObjects(ctx.doc().header())) {
                const BlockDef* annoDef = ctx.doc().block(data.block);
                const double n = currentAnnotationScale(ctx.doc().header());
                if (annoDef && annoDef->xrefPath.empty() && !data.array) {
                    e.common.annotative = true;
                    e.common.annoScales = ScaleList(std::vector<double>{n});
                    if (auto* ins = entityAs<InsertData>(e))
                        ins->scale = ins->scale * n;
                }
            }
            if (multiple)
                ctx.checkpoint();  // each copy shows at once and survives Esc
            auto added = ctx.tx().add(std::move(e));
            if (!added) {
                ctx.message(added.error().message);
                co_return;
            }
            if (explodeAfter) {
                const Entity* placedEntity = ctx.doc().find(added.value());
                if (placedEntity) {
                    auto parts = blocks::explode(ctx.doc(), *placedEntity);
                    if (parts) {
                        (void)ctx.tx().erase(added.value());
                        std::vector<EntityId> ids;
                        for (Entity& part : parts.value().entities)
                            if (auto a = ctx.tx().add(std::move(part)))
                                ids.push_back(a.value());
                        ctx.host().setSelection(ids);
                        ctx.setUndoLabel("Insert " + def->name + " exploded");
                        ctx.message("Block \"" + def->name + "\" inserted and exploded");
                        if (!multiple)
                            co_return;
                        continue;
                    }
                    ctx.message(parts.error().message);
                }
            }
            ctx.host().setSelection({added.value()});
            ctx.setUndoLabel("Insert " + def->name);
            ctx.message("Block \"" + def->name + "\" inserted" +
                        (multiple ? " (" + std::to_string(placed + 1) + ")" : ""));
            if (!multiple)
                co_return;
        }
    }

private:
    bool array_;
};

}  // namespace

std::unique_ptr<cmd::Command> makeMinsertCommand() {
    return std::make_unique<InsertCommand>(true);
}

std::unique_ptr<cmd::Command> makeInsertCommand() {
    return std::make_unique<InsertCommand>();
}

}  // namespace tbcad::block_cmds
