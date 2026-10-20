#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <sstream>

#include "common.h"
#include "tbcad/text/text.h"

namespace tbcad::select {

PurgeCounts purgeUnused(const Document& doc, Transaction& tx, unsigned kinds, bool nested) {
    PurgeCounts n;
    bool first = true;
    auto want = [&](PurgeKind k) { return (kinds & static_cast<unsigned>(k)) != 0; };
    // Removing one row can free another, so go round until a pass removes nothing. "In use" is
    // answered from one scan of the objects per pass, not one scan per row.
    for (bool progress = true; progress;) {
        progress = false;
        if (!first && !nested)
            break;
        first = false;
        tx.cacheObjectReferences(true);
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
            // A block that holds the last insert of another frees it as it goes, so keep going
            // round the blocks until none can go (cheap: no rescan of the objects).
            for (bool again = true; again;) {
                again = false;
                std::vector<BlockId> ids;
                for (const auto& [id, row] : doc.blocks())
                    if (!row.anonymous)
                        ids.push_back(id);
                for (BlockId id : ids)
                    if (tx.eraseBlock(id)) {
                        ++n.blocks;
                        progress = true;
                        again = nested;
                    }
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
    tx.cacheObjectReferences(false);
    return n;
}

namespace {

void addRows(std::vector<PurgeItem>& out, const Document& doc, unsigned kinds) {
    auto want = [&](PurgeKind k) { return (kinds & static_cast<unsigned>(k)) != 0; };
    if (want(PurgeKind::Layers))
        for (const auto& [id, row] : doc.layers())
            out.push_back({PurgeKind::Layers, id.value, row.name});
    if (want(PurgeKind::Blocks))
        for (const auto& [id, row] : doc.blocks())
            if (!row.anonymous)
                out.push_back({PurgeKind::Blocks, id.value, row.name});
    if (want(PurgeKind::TextStyles))
        for (const auto& [id, row] : doc.textStyles())
            out.push_back({PurgeKind::TextStyles, id.value, row.name});
    if (want(PurgeKind::Linetypes))
        for (const auto& [id, row] : doc.linetypes())
            out.push_back({PurgeKind::Linetypes, id.value, row.name});
    if (want(PurgeKind::DimStyles))
        for (const auto& [id, row] : doc.dimStyles())
            out.push_back({PurgeKind::DimStyles, id.value, row.name});
}

bool rowExists(const Document& doc, const PurgeItem& it) {
    switch (it.kind) {
        case PurgeKind::Layers:
            return doc.layer(LayerId{it.id}) != nullptr;
        case PurgeKind::Blocks:
            return doc.block(BlockId{it.id}) != nullptr;
        case PurgeKind::TextStyles:
            return doc.textStyle(TextStyleId{it.id}) != nullptr;
        case PurgeKind::Linetypes:
            return doc.linetype(LinetypeId{it.id}) != nullptr;
        case PurgeKind::DimStyles:
            return doc.dimStyle(DimStyleId{it.id}) != nullptr;
        default:
            return false;
    }
}

bool eraseRow(const Document& doc, Transaction& tx, const PurgeItem& it) {
    switch (it.kind) {
        case PurgeKind::Layers:
            return it.id != doc.header().currentLayer.value && tx.eraseLayer(LayerId{it.id}).ok();
        case PurgeKind::Blocks:
            return tx.eraseBlock(BlockId{it.id}).ok();
        case PurgeKind::TextStyles:
            return it.id != doc.header().currentTextStyle.value &&
                   tx.eraseTextStyle(TextStyleId{it.id}).ok();
        case PurgeKind::Linetypes:
            return tx.eraseLinetype(LinetypeId{it.id}).ok();
        case PurgeKind::DimStyles:
            return tx.eraseDimStyle(DimStyleId{it.id}).ok();
        default:
            return false;
    }
}

bool blankText(std::string_view s) {
    for (char c : s)
        if (!std::isspace(static_cast<unsigned char>(c)))
            return false;
    return true;
}

}  // namespace

std::vector<PurgeItem> purgeableItems(Document& doc, bool nested) {
    std::vector<PurgeItem> before;
    addRows(before, doc, static_cast<unsigned>(PurgeKind::All));
    std::vector<PurgeItem> out;
    {
        Transaction tx = doc.begin("purge preview");
        (void)purgeUnused(doc, tx, static_cast<unsigned>(PurgeKind::All), nested);
        for (const PurgeItem& it : before)
            if (!rowExists(doc, it))
                out.push_back(it);
        // the transaction is dropped here: nothing changes
    }
    std::stable_sort(out.begin(), out.end(), [](const PurgeItem& a, const PurgeItem& b) {
        return a.kind != b.kind ? static_cast<unsigned>(a.kind) < static_cast<unsigned>(b.kind)
                                : a.name < b.name;
    });
    return out;
}

int purgeItems(const Document& doc, Transaction& tx, const std::vector<PurgeItem>& items) {
    std::vector<PurgeItem> left = items;
    int removed = 0;
    // A block may only be free once a block that inserts it has gone, a layer once the block
    // that used it has: go round until a round removes nothing.
    for (bool progress = true; progress && !left.empty();) {
        progress = false;
        for (auto it = left.begin(); it != left.end();) {
            if (!rowExists(doc, *it)) {
                it = left.erase(it);
                continue;
            }
            if (eraseRow(doc, tx, *it)) {
                ++removed;
                progress = true;
                it = left.erase(it);
            } else {
                ++it;
            }
        }
    }
    return removed;
}

std::vector<EntityId> zeroLengthObjects(const Document& doc) {
    std::vector<EntityId> out;
    doc.forEachEntity([&](const Entity& e) {
        bool zero = false;
        if (const auto* l = entityAs<LineData>(e)) {
            zero = l->a.x == l->b.x && l->a.y == l->b.y;
        } else if (const auto* c = entityAs<CircleData>(e)) {
            zero = c->radius == 0;
        } else if (const auto* a = entityAs<ArcData>(e)) {
            zero = a->radius == 0;
        } else if (const auto* p = entityAs<PolylineData>(e)) {
            zero = true;
            for (const auto& v : p->v)
                if (v.p.x != p->v.front().p.x || v.p.y != p->v.front().p.y)
                    zero = false;
        }
        if (zero)
            out.push_back(e.common.id);
    });
    return out;
}

std::vector<EntityId> emptyTextObjects(const Document& doc) {
    std::vector<EntityId> out;
    doc.forEachEntity([&](const Entity& e) {
        if (const auto* t = entityAs<TextData>(e); t && blankText(t->utf8))
            out.push_back(e.common.id);
        else if (const auto* m = entityAs<MTextData>(e); m && blankText(text::plainText(m->utf8)))
            out.push_back(e.common.id);
    });
    return out;
}

std::vector<std::string> orphanedDataKeys(const Document& doc) {
    constexpr std::string_view kLayer = "tbcad.pstyle.layer:";
    constexpr std::string_view kEntity = "tbcad.pstyle.ent:";
    std::vector<std::string> out;
    auto idOf = [](std::string_view key, std::string_view prefix) -> std::uint64_t {
        std::uint64_t v = 0;
        for (char c : key.substr(prefix.size())) {
            if (c < '0' || c > '9')
                return 0;
            v = v * 10 + static_cast<std::uint64_t>(c - '0');
        }
        return v;
    };
    for (const auto& [key, value] : doc.header().custom) {
        if (key.rfind(kLayer, 0) == 0) {
            if (!doc.layer(LayerId{static_cast<std::uint32_t>(idOf(key, kLayer))}))
                out.push_back(key);
        } else if (key.rfind(kEntity, 0) == 0) {
            if (!doc.find(EntityId{idOf(key, kEntity)}))
                out.push_back(key);
        }
    }
    return out;
}

namespace detail {
namespace {

std::string plural(int n, const char* one) {
    return std::to_string(n) + " " + one + (n == 1 ? "" : "s");
}

// PURGE: removes layers, blocks and styles nothing uses.
class Purge final : public cmd::Command {
public:
    std::string_view name() const override { return "-PURGE"; }
    std::string_view description() const override {
        return "Remove unused layers, blocks, styles, zero-length objects and empty text";
    }

    cmd::Task run(cmd::CommandContext& ctx) override {
        cmd::KeywordOptions ko;
        ko.keywords = {"Layers",     "Blocks",    "Textstyles", "Linetypes", "Dimstyles",
                       "Zerolength", "Emptytext", "Orphaned",   "All"};
        ko.defaultKeyword = "All";
        auto k = co_await ctx.getKeyword(
            "Purge [Layers/Blocks/Textstyles/Linetypes/Dimstyles/"
            "Zerolength/Emptytext/Orphaned/All] <All>",
            std::move(ko));
        if (!k.ok())
            co_return;
        if (k.keyword == "Zerolength" || k.keyword == "Emptytext" || k.keyword == "Orphaned") {
            const bool orphaned = k.keyword == "Orphaned";
            const auto ids = k.keyword == "Zerolength" ? zeroLengthObjects(ctx.doc())
                             : orphaned                ? std::vector<EntityId>{}
                                                       : emptyTextObjects(ctx.doc());
            const auto keys = orphaned ? orphanedDataKeys(ctx.doc()) : std::vector<std::string>{};
            const int count = static_cast<int>(orphaned ? keys.size() : ids.size());
            if (count == 0) {
                ctx.message("Nothing to purge.");
                co_return;
            }
            const std::string what = plural(count, k.keyword == "Zerolength" ? "zero-length object"
                                                   : orphaned                ? "orphaned setting"
                                                                             : "empty text object");
            const std::string prompt = "Remove " + what + "? [Yes/No] <No>";
            auto yes = co_await ctx.getKeyword(
                prompt, cmd::KeywordOptions{.keywords = {"Yes", "No"}, .defaultKeyword = "No"});
            if (!yes.ok() || yes.keyword != "Yes") {
                ctx.message("Nothing was purged.");
                co_return;
            }
            if (orphaned) {
                (void)ctx.tx().modifyHeader([&](Header& h) {
                    for (const std::string& key : keys)
                        h.custom.erase(key);
                });
            } else {
                for (EntityId id : ids)
                    (void)ctx.tx().erase(id);
            }
            ctx.message("Purged " + what + ".");
            co_return;
        }
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
        std::vector<std::string> layerNames, blockNames;
        for (const auto& [id, row] : ctx.doc().layers())
            layerNames.push_back(row.name);
        for (const auto& [id, row] : ctx.doc().blocks())
            if (!row.anonymous)
                blockNames.push_back(row.name);
        ctx.checkpoint();
        const PurgeCounts n = purgeUnused(ctx.doc(), ctx.tx(), kinds);
        if (n.total() == 0) {
            ctx.message("Nothing to purge.");
            co_return;
        }
        // Show what would go and ask first: a layer made a moment ago is "unused" too.
        {
            std::string names;
            int gone = 0;
            for (const std::string& name : layerNames)
                if (!ctx.doc().layerByName(name) && ++gone <= 6)
                    names += (names.empty() ? "" : ", ") + name;
            for (const std::string& name : blockNames)
                if (!ctx.doc().blockByName(name) && ++gone <= 6)
                    names += (names.empty() ? "" : ", ") + name;
            std::string what;
            auto part = [&](int count, const char* word) {
                if (count)
                    what += (what.empty() ? "" : ", ") + plural(count, word);
            };
            part(n.layers, "layer");
            part(n.blocks, "block");
            part(n.textStyles, "text style");
            part(n.linetypes, "linetype");
            part(n.dimStyles, "dimension style");
            if (!names.empty())
                ctx.message("Unused: " + names + (gone > 6 ? ", ..." : "") + ".");
            auto yes = co_await ctx.getKeyword(
                "Remove " + what + "? [Yes/No] <No>",
                cmd::KeywordOptions{.keywords = {"Yes", "No"}, .defaultKeyword = "No"});
            if (!yes.ok() || yes.keyword != "Yes") {
                ctx.rollbackToCheckpoint();
                ctx.message("Nothing was purged.");
                co_return;
            }
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
        // Name what went, so a layer that vanished is not a mystery (Undo brings it back).
        auto list = [&](const char* what, const std::vector<std::string>& before, auto exists) {
            std::string names;
            int gone = 0;
            for (const std::string& name : before) {
                if (exists(name))
                    continue;
                if (++gone <= 8)
                    names += (names.empty() ? "" : ", ") + name;
            }
            if (gone)
                ctx.message(std::string(what) + ": " + names + (gone > 8 ? ", ..." : "") +
                            ". Undo brings them back.");
        };
        list("Layers purged", layerNames,
             [&](const std::string& nm) { return ctx.doc().layerByName(nm) != nullptr; });
        list("Blocks purged", blockNames,
             [&](const std::string& nm) { return ctx.doc().blockByName(nm) != nullptr; });
    }
};

// _PURGEITEMS: what the Purge dialog chose, from the setting "tmp.purge.items" (a list like
// "L12 B3 Z E O": layer 12, block 3, zero-length objects, empty text, orphaned data), in one undo
// step. Not for typing.
class PurgeItemsCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "_PURGEITEMS"; }
    std::string_view description() const override { return {}; }
    cmd::Task run(cmd::CommandContext& ctx) override {
        const std::string spec = ctx.host().setting("tmp.purge.items").value_or("");
        ctx.host().setSetting("tmp.purge.items", "");
        ctx.setUndoLabel("PURGE");
        std::vector<PurgeItem> rows;
        bool zero = false, empty = false, orphaned = false;
        std::istringstream in(spec);
        for (std::string tok; in >> tok;) {
            if (tok == "Z") {
                zero = true;
                continue;
            }
            if (tok == "E") {
                empty = true;
                continue;
            }
            if (tok == "O") {
                orphaned = true;
                continue;
            }
            PurgeItem it;
            switch (tok[0]) {
                case 'L':
                    it.kind = PurgeKind::Layers;
                    break;
                case 'B':
                    it.kind = PurgeKind::Blocks;
                    break;
                case 'T':
                    it.kind = PurgeKind::TextStyles;
                    break;
                case 'Y':
                    it.kind = PurgeKind::Linetypes;
                    break;
                case 'D':
                    it.kind = PurgeKind::DimStyles;
                    break;
                default:
                    continue;
            }
            it.id = static_cast<std::uint32_t>(std::strtoul(tok.c_str() + 1, nullptr, 10));
            rows.push_back(it);
        }
        int objects = 0;
        if (zero)
            for (EntityId id : zeroLengthObjects(ctx.doc()))
                objects += ctx.tx().erase(id).ok() ? 1 : 0;
        if (empty)
            for (EntityId id : emptyTextObjects(ctx.doc()))
                objects += ctx.tx().erase(id).ok() ? 1 : 0;
        // Rows after the objects: a layer may only have held empty text.
        const int named = purgeItems(ctx.doc(), ctx.tx(), rows);
        int settings = 0;
        if (orphaned) {
            const auto keys = orphanedDataKeys(ctx.doc());
            settings = static_cast<int>(keys.size());
            if (!keys.empty())
                (void)ctx.tx().modifyHeader([&](Header& h) {
                    for (const std::string& key : keys)
                        h.custom.erase(key);
                });
        }
        if (named + objects + settings == 0) {
            ctx.message("Nothing was purged.");
            co_return;
        }
        std::string msg = "Purged";
        if (named)
            msg += " " + plural(named, "named object") + ",";
        if (objects)
            msg += " " + plural(objects, "object") + ",";
        if (settings)
            msg += " " + plural(settings, "orphaned setting") + ",";
        msg.back() = '.';
        ctx.message(msg + " Undo brings them back.");
    }
};

// PURGE: the Purge dialog (what can go, with previews of the blocks); -PURGE is the typed form.
class PurgeDialog final : public cmd::Command {
public:
    std::string_view name() const override { return "PURGE"; }
    std::vector<std::string> defaultAliases() const override { return {"PU"}; }
    std::string_view description() const override {
        return "Open the Purge dialog: remove unused named objects, zero-length objects and empty "
               "text";
    }
    cmd::Task run(cmd::CommandContext& ctx) override {
        ctx.host().openPanel("purge");
        co_return;
    }
};

}  // namespace

std::unique_ptr<cmd::Command> makePurge() {
    return std::make_unique<Purge>();
}

std::unique_ptr<cmd::Command> makePurgeDialog() {
    return std::make_unique<PurgeDialog>();
}

std::unique_ptr<cmd::Command> makePurgeItems() {
    return std::make_unique<PurgeItemsCommand>();
}

}  // namespace detail
}  // namespace tbcad::select
