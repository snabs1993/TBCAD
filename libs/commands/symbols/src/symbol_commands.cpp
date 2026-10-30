#include <cmath>

#include "tbcad/blocks/attributes.h"
#include "tbcad/cmd/cmd.h"
#include "tbcad/symbolscmd/symbolscmd.h"
#include "tbcad/ui/ui_registry.h"

namespace tbcad::symbolscmd {

namespace {

// One command per symbol: NORTHARROW, GRIDBUBBLE, LEVELMARK, SECTIONMARK, ELEVATIONMARK,
// DETAILMARK. The block is made on first use, the symbol goes in as an annotative insert at the
// current annotation scale, and the attributes (grid letter, level, section letter, sheet ...)
// are asked for one by one.
class SymbolCommand final : public cmd::Command {
public:
    explicit SymbolCommand(const SymbolInfo& info) : info_(info) {}
    std::string_view name() const override { return info_.command; }
    std::vector<std::string> defaultAliases() const override { return {info_.alias}; }
    std::string_view description() const override { return info_.title; }

    cmd::Task run(cmd::CommandContext& ctx) override {
        auto p = co_await ctx.getPoint(std::string("Specify ") + info_.title + " position");
        if (!p.ok())
            co_return;
        cmd::AngleOptions ao;
        ao.base = p.point;
        ao.defaultValue = 0.0;
        auto a = co_await ctx.getAngle("Specify rotation angle", ao);
        if (!a.ok())
            co_return;

        auto block = ensureSymbolBlock(ctx.tx(), ctx.doc(), info_.symbol);
        if (!block) {
            ctx.message(block.error().message);
            co_return;
        }
        InsertData data{block.value(), p.point, {1, 1}, a.value};
        data.attribs = blocks::defaultAttribs(ctx.doc(), block.value());
        // The attribute prompts come from the block definition read back from the document, so a
        // symbol the user edited keeps asking what its attributes say.
        for (AttribValue& v : data.attribs) {
            cmd::StringOptions so;
            so.defaultValue = v.value;
            so.allowEmpty = true;
            auto t = co_await ctx.getString(
                (promptOf(ctx.doc(), block.value(), v.tag)) + " <" + v.value + ">", so);
            if (!t.ok())
                co_return;
            if (!t.text.empty())
                v.value = t.text;
        }

        const double n = currentAnnotationScale(ctx.doc().header());
        data.scale = {n, n};
        Entity e;
        e.data = Indirect<InsertData>(std::move(data));
        applyCurrentProperties(ctx.doc().header(), e);
        e.common.annotative = true;
        e.common.annoScales = ScaleList(std::vector<double>{n});
        auto id = ctx.tx().add(std::move(e));
        if (!id) {
            ctx.message(id.error().message);
            co_return;
        }
        ctx.setUndoLabel(info_.title);
    }

private:
    static std::string promptOf(const Document& doc, BlockId block, const std::string& tag) {
        if (const BlockDef* def = doc.block(block)) {
            for (EntityId id : def->entities) {
                const Entity* e = doc.find(id);
                const auto* t = e ? entityAs<TextData>(*e) : nullptr;
                if (t && t->attdef && t->attdef->tag == tag && !t->attdef->prompt.empty())
                    return t->attdef->prompt;
            }
        }
        return tag;
    }

    SymbolInfo info_;
};

}  // namespace

void registerToleranceCommand(cmd::Registry& registry);

void registerCommands(cmd::Registry& registry) {
    for (const SymbolInfo& info : symbolList())
        (void)registry.add(std::make_unique<SymbolCommand>(info));
    registerToleranceCommand(registry);
}

void registerUi(ui::UiRegistry& ui) {
    for (const SymbolInfo& info : symbolList())
        ui.addMenuItem(std::string("Draw/Symbols/") + info.title, info.command);
    ui.addMenuItem("Dimension/Tolerance", "TOLERANCE");
}

}  // namespace tbcad::symbolscmd
