#include <cmath>

#include "commands.h"
#include "tbcad/blocks/tbcad_blocks.h"

namespace tbcad::block_cmds {

namespace {

constexpr const char* kLastBlockSetting = "block.lastinsert";
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

// INSERT [I]: places an instance of a block with a live preview.
class InsertCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "INSERT"; }
    std::vector<std::string> defaultAliases() const override { return {"I"}; }
    std::string_view description() const override { return "Insert a block"; }

    cmd::Task run(cmd::CommandContext& ctx) override {
        Document& doc = ctx.doc();
        if (doc.blocks().empty()) {
            ctx.message("There are no blocks in this drawing. Use BLOCK to make one.");
            co_return;
        }

        // Block name.
        const BlockDef* def = nullptr;
        for (;;) {
            cmd::StringOptions so;
            if (auto last = ctx.host().setting(kLastBlockSetting); last && doc.blockByName(*last))
                so.defaultValue = *last;
            auto n = co_await ctx.getString("Enter block name or [?]", std::move(so));
            if (!n.ok())
                co_return;
            std::string text = cmd::trim(n.text);
            if (text == "?") {
                ctx.message("Blocks: " + blockNames(doc));
                continue;
            }
            def = doc.blockByName(text);
            if (def)
                break;
            ctx.message("No block named \"" + text + "\". Blocks: " + blockNames(doc));
        }
        const BlockId block = def->id;
        ctx.host().setSetting(kLastBlockSetting, def->name);

        // Placement. Scale and rotation can be changed before the point is picked.
        InsertData data{block, {}, {1, 1}, 0};
        data.attribs = blocks::defaultAttribs(doc, block);
        for (;;) {
            cmd::PointOptions po;
            po.keywords = {"Scale", "X", "Y", "Rotate"};
            po.allowNone = false;
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
            auto p = co_await ctx.getPoint("Specify insertion point or [Scale/X/Y/Rotate]",
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
            } else if (p.keyword == "Rotate") {
                auto a = co_await ctx.getAngle("Specify rotation angle <0>",
                                               cmd::AngleOptions{.defaultValue = 0.0});
                if (!a.ok())
                    co_return;
                data.rotation = normalizeAngle(a.value);
            }
        }

        // Attribute values. Preset and constant attributes are not asked for.
        if (ctx.host().setting(kAttReqSetting).value_or("1") != "0") {
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
        auto added = ctx.tx().add(std::move(e));
        if (!added) {
            ctx.message(added.error().message);
            co_return;
        }
        ctx.host().setSelection({added.value()});
        ctx.setUndoLabel("Insert " + def->name);
    }
};

}  // namespace

std::unique_ptr<cmd::Command> makeInsertCommand() {
    return std::make_unique<InsertCommand>();
}

}  // namespace tbcad::block_cmds
