#include "tbcad/clipboard/clipboard.h"

#include <functional>
#include <mutex>
#include <type_traits>

#include "tbcad/geom/geom.h"

namespace tbcad::clipboard {

namespace {

// How the ids an entity refers to are translated. Capture uses it with recording identity
// functions, paste with functions that find or create the matching row in the target drawing.
struct RefMap {
    std::function<LayerId(LayerId)> layer;
    std::function<LevelId(LevelId)> level;
    std::function<LinetypeId(LinetypeId)> linetype;
    std::function<TextStyleId(TextStyleId)> text;
    std::function<DimStyleId(DimStyleId)> dim;
    std::function<BlockId(BlockId)> block;
};

void remap(Entity& e, const RefMap& m) {
    e.common.layer = m.layer(e.common.layer);
    e.common.level = m.level(e.common.level);
    e.common.linetype = m.linetype(e.common.linetype);
    visitEntity(e, [&](auto& d) {
        using D = std::decay_t<decltype(d)>;
        if constexpr (requires { d.style; }) {
            using S = std::decay_t<decltype(d.style)>;
            if constexpr (std::is_same_v<S, TextStyleId>)
                d.style = m.text(d.style);
            else if constexpr (std::is_same_v<S, DimStyleId>)
                d.style = m.dim(d.style);
        }
        if constexpr (std::is_same_v<D, InsertData>)
            d.block = m.block(d.block);
        if constexpr (std::is_same_v<D, HatchData>)
            d.sourceBoundary.clear();
    });
}

bool isSpecialLinetype(LinetypeId id) {
    return id == kLinetypeByLayer || id == kLinetypeByBlock || id == kLinetypeByParent ||
           id == kLinetypeContinuous;
}

// ---- capture ------------------------------------------------------------------------------------

class Collector {
public:
    Collector(const Document& doc, Clip& clip) : doc_(doc), clip_(clip) {}

    void collect(Entity& e) { remap(e, map_); }

private:
    LayerId layer(LayerId id) {
        if (!id || clip_.layers.count(id))
            return id;
        const Layer* row = doc_.layer(id);
        if (!row)
            return id;
        clip_.layers[id] = *row;
        layer(row->parent);
        linetype(row->linetype);
        return id;
    }
    LevelId level(LevelId id) {
        if (static_cast<bool>(id) && !clip_.levels.count(id)) {
            if (const Level* row = doc_.level(id))
                clip_.levels[id] = *row;
        }
        return id;
    }
    LinetypeId linetype(LinetypeId id) {
        if (!isSpecialLinetype(id) && !clip_.linetypes.count(id)) {
            if (const Linetype* row = doc_.linetype(id))
                clip_.linetypes[id] = *row;
        }
        return id;
    }
    TextStyleId text(TextStyleId id) {
        if (static_cast<bool>(id) && !clip_.textStyles.count(id)) {
            if (const TextStyle* row = doc_.textStyle(id))
                clip_.textStyles[id] = *row;
        }
        return id;
    }
    DimStyleId dim(DimStyleId id) {
        if (static_cast<bool>(id) && !clip_.dimStyles.count(id)) {
            if (const DimStyle* row = doc_.dimStyle(id)) {
                clip_.dimStyles[id] = *row;
                text(row->textStyle);
            }
        }
        return id;
    }
    BlockId block(BlockId id) {
        if (!id || clip_.blocks.count(id))
            return id;
        const BlockDef* row = doc_.block(id);
        if (!row)
            return id;
        Clip::BlockCopy& copy = clip_.blocks[id];
        copy.def = *row;
        copy.def.entities.clear();
        std::vector<Entity> members;
        for (EntityId eid : row->entities) {
            if (const Entity* src = doc_.find(eid)) {
                members.push_back(*src);
                remap(members.back(), map_);
            }
        }
        clip_.blocks[id].entities = std::move(members);  // `copy` may have moved with the map
        return id;
    }

    const Document& doc_;
    Clip& clip_;
    RefMap map_{
        [this](LayerId i) { return layer(i); },       [this](LevelId i) { return level(i); },
        [this](LinetypeId i) { return linetype(i); }, [this](TextStyleId i) { return text(i); },
        [this](DimStyleId i) { return dim(i); },      [this](BlockId i) { return block(i); }};
};

// ---- paste --------------------------------------------------------------------------------------

class Importer {
public:
    Importer(Transaction& tx, const Clip& clip) : tx_(tx), clip_(clip) {}

    Entity import(const Entity& src) {
        Entity e = src;
        e.common.id = {};
        remap(e, map_);
        return e;
    }
    const std::string& error() const { return error_; }
    std::vector<std::pair<LayerId, std::string>> newLayerStyles_{};

    BlockId newBlock(const std::string& name, Vec2 base) {
        BlockDef def;
        def.name = name;
        def.base = base;
        auto r = tx_.addBlock(std::move(def));
        if (!r) {
            fail(r.error().message);
            return {};
        }
        return r.value();
    }

private:
    void fail(const std::string& m) {
        if (error_.empty())
            error_ = m;
    }
    const Document& doc() const { return tx_.document(); }

    LayerId layer(LayerId id) {
        if (!id)
            return id;
        if (auto it = layers_.find(id); it != layers_.end())
            return it->second;
        auto src = clip_.layers.find(id);
        if (src == clip_.layers.end())
            return layers_[id] = id;
        LayerId parent = layer(src->second.parent);
        LayerId result{};
        if (const Layer* found = doc().layerByName(parent, src->second.name)) {
            result = found->id;
        } else {
            Layer row = src->second;
            row.id = {};
            row.parent = parent;
            row.linetype = linetype(row.linetype);
            auto r = tx_.addLayer(std::move(row));
            if (r) {
                result = r.value();
                // A layer made here brings the plot style name it had.
                if (auto st = clip_.layerPlotStyles.find(id); st != clip_.layerPlotStyles.end())
                    newLayerStyles_.emplace_back(result, st->second);
            } else {
                fail(r.error().message);
            }
        }
        return layers_[id] = result;
    }
    LevelId level(LevelId id) {
        if (!id)
            return id;
        if (auto it = levels_.find(id); it != levels_.end())
            return it->second;
        auto src = clip_.levels.find(id);
        if (src == clip_.levels.end())
            return levels_[id] = LevelId{};
        LevelId result{};
        if (const Level* found = doc().levelByName(src->second.name)) {
            result = found->id;
        } else {
            Level row = src->second;
            row.id = {};
            auto r = tx_.addLevel(std::move(row));
            if (r)
                result = r.value();
            else
                fail(r.error().message);
        }
        return levels_[id] = result;
    }
    LinetypeId linetype(LinetypeId id) {
        if (isSpecialLinetype(id))
            return id;
        if (auto it = linetypes_.find(id); it != linetypes_.end())
            return it->second;
        auto src = clip_.linetypes.find(id);
        if (src == clip_.linetypes.end())
            return linetypes_[id] = kLinetypeByLayer;
        LinetypeId result = kLinetypeByLayer;
        if (const Linetype* found = doc().linetypeByName(src->second.name)) {
            result = found->id;
        } else {
            Linetype row = src->second;
            row.id = {};
            auto r = tx_.addLinetype(std::move(row));
            if (r)
                result = r.value();
            else
                fail(r.error().message);
        }
        return linetypes_[id] = result;
    }
    TextStyleId text(TextStyleId id) {
        if (!id)
            return id;
        if (auto it = texts_.find(id); it != texts_.end())
            return it->second;
        auto src = clip_.textStyles.find(id);
        if (src == clip_.textStyles.end())
            return texts_[id] = TextStyleId{};
        TextStyleId result{};
        if (const TextStyle* found = doc().textStyleByName(src->second.name)) {
            result = found->id;
        } else {
            TextStyle row = src->second;
            row.id = {};
            auto r = tx_.addTextStyle(std::move(row));
            if (r)
                result = r.value();
            else
                fail(r.error().message);
        }
        return texts_[id] = result;
    }
    DimStyleId dim(DimStyleId id) {
        if (!id)
            return id;
        if (auto it = dims_.find(id); it != dims_.end())
            return it->second;
        auto src = clip_.dimStyles.find(id);
        if (src == clip_.dimStyles.end())
            return dims_[id] = DimStyleId{};
        DimStyleId result{};
        if (const DimStyle* found = doc().dimStyleByName(src->second.name)) {
            result = found->id;
        } else {
            DimStyle row = src->second;
            row.id = {};
            row.textStyle = text(row.textStyle);
            auto r = tx_.addDimStyle(std::move(row));
            if (r)
                result = r.value();
            else
                fail(r.error().message);
        }
        return dims_[id] = result;
    }
    BlockId block(BlockId id) {
        if (!id)
            return id;
        if (auto it = blocks_.find(id); it != blocks_.end())
            return it->second;
        auto src = clip_.blocks.find(id);
        if (src == clip_.blocks.end())
            return blocks_[id] = id;
        if (const BlockDef* found = doc().blockByName(src->second.def.name))
            return blocks_[id] = found->id;  // the drawing's own definition wins
        BlockDef def = src->second.def;
        def.id = {};
        def.entities.clear();
        auto r = tx_.addBlock(std::move(def));
        if (!r) {
            fail(r.error().message);
            return blocks_[id] = BlockId{};
        }
        BlockId created = r.value();
        blocks_[id] = created;  // before the members: nested inserts may refer back
        for (const Entity& member : src->second.entities) {
            Entity e = import(member);
            if (auto a = tx_.add(std::move(e), created); !a)
                fail(a.error().message);
        }
        return created;
    }

    Transaction& tx_;
    const Clip& clip_;
    std::string error_{};
    std::map<LayerId, LayerId> layers_{};
    std::map<LevelId, LevelId> levels_{};
    std::map<LinetypeId, LinetypeId> linetypes_{};
    std::map<TextStyleId, TextStyleId> texts_{};
    std::map<DimStyleId, DimStyleId> dims_{};
    std::map<BlockId, BlockId> blocks_{};
    RefMap map_{
        [this](LayerId i) { return layer(i); },       [this](LevelId i) { return level(i); },
        [this](LinetypeId i) { return linetype(i); }, [this](TextStyleId i) { return text(i); },
        [this](DimStyleId i) { return dim(i); },      [this](BlockId i) { return block(i); }};
};

std::mutex g_mutex;
Clip g_clip;

}  // namespace

Clip capture(const Document& doc, std::span<const EntityId> ids, std::optional<Vec2> base) {
    Clip clip;
    Collector collector(doc, clip);
    for (EntityId id : ids) {
        const Entity* e = doc.find(id);
        if (!e || static_cast<bool>(doc.ownerBlock(id)))
            continue;
        clip.bounds.merge(geom::bounds(*e, doc));
        clip.entities.push_back(*e);
        clip.plotStyles.push_back(entityPlotStyle(doc.header(), id));
        collector.collect(clip.entities.back());
    }
    for (const auto& [id, row] : clip.layers) {
        (void)row;
        if (std::string st = layerPlotStyle(doc.header(), id); !st.empty())
            clip.layerPlotStyles[id] = std::move(st);
    }
    clip.base = base ? *base : (clip.bounds.empty() ? Vec2{} : clip.bounds.min);
    return clip;
}

Result<std::vector<EntityId>> paste(Transaction& tx, const Clip& clip, const PasteOptions& opts) {
    if (clip.empty())
        return Error{ErrorCode::InvalidArgument, "The clipboard is empty."};
    Importer in(tx, clip);
    std::vector<EntityId> created;
    std::vector<std::pair<EntityId, std::string>> entityStyles;  // new entity, own style name
    auto styleOf = [&](std::size_t i) {
        return i < clip.plotStyles.size() ? clip.plotStyles[i] : std::string();
    };
    if (opts.asBlock) {
        std::string name;
        for (int n = 1;; ++n) {
            name = "A$C" + std::to_string(n);
            if (!tx.document().blockByName(name))
                break;
        }
        BlockId block = in.newBlock(name, clip.base);
        if (!block)
            return Error{ErrorCode::InvalidArgument, in.error()};
        for (std::size_t i = 0; i < clip.entities.size(); ++i) {
            Entity e = in.import(clip.entities[i]);
            if (opts.layer)
                e.common.layer = opts.layer;
            auto r = tx.add(std::move(e), block);
            if (!r)
                return r.error();
            if (std::string st = styleOf(i); !st.empty())
                entityStyles.emplace_back(r.value(), std::move(st));
        }
        Entity ins;
        InsertData d;
        d.block = block;
        d.position = opts.target;
        ins.data = std::move(d);
        if (opts.layer)
            ins.common.layer = opts.layer;
        auto r = tx.add(std::move(ins));
        if (!r)
            return r.error();
        created.push_back(r.value());
    } else {
        Transform2 move = Transform2::translate(opts.target - clip.base);
        for (std::size_t i = 0; i < clip.entities.size(); ++i) {
            Entity e = in.import(clip.entities[i]);
            if (opts.layer)
                e.common.layer = opts.layer;
            auto moved = geom::transformed(e, move);
            if (!moved)
                continue;
            auto r = tx.add(std::move(moved.value()));
            if (!r)
                return r.error();
            created.push_back(r.value());
            if (std::string st = styleOf(i); !st.empty())
                entityStyles.emplace_back(r.value(), std::move(st));
        }
    }
    if (!in.error().empty())
        return Error{ErrorCode::InvalidArgument, in.error()};
    if (!entityStyles.empty() || !in.newLayerStyles_.empty()) {
        (void)tx.modifyHeader([&](Header& h) {
            for (const auto& [layer, st] : in.newLayerStyles_)
                setLayerPlotStyle(h, layer, st);
            for (const auto& [id, st] : entityStyles)
                setEntityPlotStyle(h, id, st);
        });
    }
    return created;
}

const Clip& current() {
    return g_clip;
}

void setCurrent(Clip clip) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_clip = std::move(clip);
}

bool hasClip() {
    return !g_clip.empty();
}

}  // namespace tbcad::clipboard
