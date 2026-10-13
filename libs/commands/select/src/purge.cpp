#include "common.h"

namespace tbcad::select {

PurgeCounts purgeUnused(const Document& doc, Transaction& tx, unsigned kinds) {
    PurgeCounts n;
    auto want = [&](PurgeKind k) { return (kinds & static_cast<unsigned>(k)) != 0; };
    // Removing one row can free another, so go round until a pass removes nothing.
    for (bool progress = true; progress;) {
        progress = false;
        if (want(PurgeKind::DimStyles)) {
            std::vector<DimStyleId> ids;
            for (const auto& [id, row] : doc.dimStyles())
                ids.push_back(id);
            for (DimStyleId id : ids)
                if (tx.eraseDimStyle(id)) {
                    ++n.dimStyles;
                    progress = true;
                }
        }
        if (want(PurgeKind::Blocks)) {
            std::vector<BlockId> ids;
            for (const auto& [id, row] : doc.blocks())
                if (!row.anonymous)
                    ids.push_back(id);
            for (BlockId id : ids)
                if (tx.eraseBlock(id)) {
                    ++n.blocks;
                    progress = true;
                }
        }
        if (want(PurgeKind::Layers)) {
            std::vector<LayerId> ids;
            for (const auto& [id, row] : doc.layers())
                if (id != doc.header().currentLayer)
                    ids.push_back(id);
            for (LayerId id : ids)
                if (tx.eraseLayer(id)) {
                    ++n.layers;
                    progress = true;
                }
        }
        if (want(PurgeKind::TextStyles)) {
            std::vector<TextStyleId> ids;
            for (const auto& [id, row] : doc.textStyles())
                if (id != doc.header().currentTextStyle)
                    ids.push_back(id);
            for (TextStyleId id : ids)
                if (tx.eraseTextStyle(id)) {
                    ++n.textStyles;
                    progress = true;
                }
        }
        if (want(PurgeKind::Linetypes)) {
            std::vector<LinetypeId> ids;
            for (const auto& [id, row] : doc.linetypes())
                ids.push_back(id);
            for (LinetypeId id : ids)
                if (tx.eraseLinetype(id)) {
                    ++n.linetypes;
                    progress = true;
                }
        }
    }
    return n;
}

namespace detail {
namespace {

std::string plural(int n, const char* one) {
    return std::to_string(n) + " " + one + (n == 1 ? "" : "s");
}

// PURGE: removes layers, blocks and styles nothing uses.
class Purge final : public cmd::Command {
public:
    std::string_view name() const override { return "PURGE"; }
    std::vector<std::string> defaultAliases() const override { return {"PU"}; }
    std::string_view description() const override {
        return "Remove unused layers, blocks, styles and linetypes";
    }

    cmd::Task run(cmd::CommandContext& ctx) override {
        cmd::KeywordOptions ko;
        ko.keywords = {"Layers", "Blocks", "Textstyles", "Linetypes", "Dimstyles", "All"};
        ko.defaultKeyword = "All";
        auto k = co_await ctx.getKeyword(
            "Purge [Layers/Blocks/Textstyles/Linetypes/Dimstyles/All] <All>", std::move(ko));
        if (!k.ok())
            co_return;
        unsigned kinds = static_cast<unsigned>(PurgeKind::All);
        if (k.keyword == "Layers")
            kinds = static_cast<unsigned>(PurgeKind::Layers);
        else if (k.keyword == "Blocks")
            kinds = static_cast<unsigned>(PurgeKind::Blocks);
        else if (k.keyword == "Textstyles")
            kinds = static_cast<unsigned>(PurgeKind::TextStyles);
        else if (k.keyword == "Linetypes")
            kinds = static_cast<unsigned>(PurgeKind::Linetypes);
        else if (k.keyword == "Dimstyles")
            kinds = static_cast<unsigned>(PurgeKind::DimStyles);
        const PurgeCounts n = purgeUnused(ctx.doc(), ctx.tx(), kinds);
        if (n.total() == 0) {
            ctx.message("Nothing to purge.");
            co_return;
        }
        std::string msg = "Purged";
        auto add = [&](int count, const char* what) {
            if (count)
                msg += " " + plural(count, what) + ",";
        };
        add(n.layers, "layer");
        add(n.blocks, "block");
        add(n.textStyles, "text style");
        add(n.linetypes, "linetype");
        add(n.dimStyles, "dimension style");
        msg.back() = '.';
        ctx.message(msg);
    }
};

}  // namespace

std::unique_ptr<cmd::Command> makePurge() {
    return std::make_unique<Purge>();
}

}  // namespace detail
}  // namespace tbcad::select
