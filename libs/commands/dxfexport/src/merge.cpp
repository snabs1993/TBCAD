#include <algorithm>
#include <map>
#include <unordered_map>

#include "tbcad/dxfexport/dxfexport.h"

namespace tbcad::dxfexport {

namespace {

struct Maps {
    std::unordered_map<LayerId, LayerId> layer{};
    std::unordered_map<LevelId, LevelId> level{};
    std::unordered_map<LinetypeId, LinetypeId> linetype{};
    std::unordered_map<TextStyleId, TextStyleId> textStyle{};
    std::unordered_map<DimStyleId, DimStyleId> dimStyle{};
    std::unordered_map<BlockId, BlockId> block{};

    template <class Id>
    static Id get(const std::unordered_map<Id, Id>& m, Id id) {
        auto it = m.find(id);
        return it == m.end() ? id : it->second;
    }
};

// Ids with a special meaning (null, ByLayer, ByBlock, ByParent) have no row and stay as they are.
void remap(Entity& e, const Maps& m) {
    e.common.layer = Maps::get(m.layer, e.common.layer);
    e.common.level = Maps::get(m.level, e.common.level);
    e.common.linetype = Maps::get(m.linetype, e.common.linetype);
    if (auto* t = entityAs<TextData>(e))
        t->style = Maps::get(m.textStyle, t->style);
    else if (auto* mt = entityAs<MTextData>(e))
        mt->style = Maps::get(m.textStyle, mt->style);
    else if (auto* in = entityAs<InsertData>(e)) {
        in->block = Maps::get(m.block, in->block);
        if (in->array)
            for (ArrayItemSwap& sw : in->array->swaps)
                sw.block = Maps::get(m.block, sw.block);
    } else if (auto* d = entityAs<DimensionData>(e))
        d->style = Maps::get(m.dimStyle, d->style);
    else if (auto* l = entityAs<LeaderData>(e))
        l->style = Maps::get(m.dimStyle, l->style);
    else if (auto* ml = entityAs<MLeaderData>(e)) {
        ml->style = Maps::get(m.dimStyle, ml->style);
        ml->block = Maps::get(m.block, ml->block);
    } else if (auto* h = entityAs<HatchData>(e))
        h->sourceBoundary.clear();  // the boundary objects keep their old ids
    else if (auto* p = entityAs<ProxyData>(e)) {
        for (auto& item : p->graphics.items) {
            std::visit([&](auto& it) { it.style.layer = Maps::get(m.layer, it.style.layer); },
                       item);
        }
    }
}

std::size_t depth(const Document& d, const Layer& l) {
    std::size_t n = 0;
    for (const Layer* p = &l; p->parent; p = d.layer(p->parent))
        ++n;
    return n;
}

}  // namespace

#define TRY(var, expr) \
    auto var = (expr); \
    if (!var)          \
    return var.error()

Result<MergeSummary> mergeDocument(const Document& dst, Transaction& tx, const Document& src) {
    MergeSummary sum;
    Maps m;

    for (const auto& [id, lt] : src.linetypes()) {
        if (const Linetype* have = dst.linetypeByName(lt.name)) {
            m.linetype[id] = have->id;
        } else {
            TRY(n, tx.addLinetype(lt));
            m.linetype[id] = n.value();
        }
    }
    for (const auto& [id, ts] : src.textStyles()) {
        if (const TextStyle* have = dst.textStyleByName(ts.name)) {
            m.textStyle[id] = have->id;
        } else {
            TRY(n, tx.addTextStyle(ts));
            m.textStyle[id] = n.value();
        }
    }
    for (const auto& [id, ds] : src.dimStyles()) {
        if (const DimStyle* have = dst.dimStyleByName(ds.name)) {
            m.dimStyle[id] = have->id;
        } else {
            DimStyle row = ds;
            row.textStyle = Maps::get(m.textStyle, row.textStyle);
            TRY(n, tx.addDimStyle(std::move(row)));
            m.dimStyle[id] = n.value();
        }
    }

    // Levels are matched by name; missing ones are added.
    for (const auto& [id, lv] : src.levels()) {
        if (const Level* have = dst.levelByName(lv.name)) {
            m.level[id] = have->id;
        } else {
            TRY(n, tx.addLevel(lv));
            m.level[id] = n.value();
        }
    }

    // Parents before children.
    std::vector<const Layer*> layers;
    for (const auto& [id, l] : src.layers())
        layers.push_back(&l);
    std::stable_sort(layers.begin(), layers.end(), [&](const Layer* a, const Layer* b) {
        return depth(src, *a) < depth(src, *b);
    });
    for (const Layer* l : layers) {
        LayerId parent = Maps::get(m.layer, l->parent);
        const Layer* have = parent ? dst.layerByName(parent, l->name) : dst.layerByName(l->name);
        if (have) {
            m.layer[l->id] = have->id;
            continue;
        }
        Layer row = *l;
        row.parent = parent;
        row.linetype = Maps::get(m.linetype, row.linetype);
        TRY(n, tx.addLayer(std::move(row)));
        m.layer[l->id] = n.value();
        ++sum.layers;
    }

    // Blocks first (inserts refer to them), then their contents.
    for (const auto& [id, b] : src.blocks()) {
        BlockDef row = b;
        row.entities.clear();
        for (int k = 1; dst.blockByName(row.name); ++k)
            row.name = b.name + "_" + std::to_string(k);
        TRY(n, tx.addBlock(std::move(row)));
        m.block[id] = n.value();
        ++sum.blocks;
    }
    for (const auto& [id, b] : src.blocks()) {
        std::map<EntityId, EntityId> memberIds;
        for (EntityId eid : b.entities) {
            const Entity* e = src.find(eid);
            if (!e)
                continue;
            Entity copy = *e;
            remap(copy, m);
            TRY(n, tx.add(std::move(copy), m.block[id]));
            memberIds[eid] = n.value();
            ++sum.entities;
        }
        if (b.dynamic || b.assembly) {
            const BlockDef& from = b;
            (void)tx.modifyBlock(m.block[id], [&](BlockDef& d) {
                d.dynamic = from.dynamic;
                d.assembly = from.assembly;
                remapBlockEntityRefs(d, [&](EntityId old) {
                    auto it = memberIds.find(old);
                    return it == memberIds.end() ? EntityId{} : it->second;
                });
            });
        }
    }
    for (EntityId eid : src.modelSpace()) {
        const Entity* e = src.find(eid);
        if (!e)
            continue;
        Entity copy = *e;
        remap(copy, m);
        TRY(n, tx.add(std::move(copy)));
        ++sum.entities;
    }
    return sum;
}

}  // namespace tbcad::dxfexport
