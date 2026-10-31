#pragma once

#include <map>
#include <memory>
#include <tuple>
#include <unordered_map>

#include "tbcad/core/core.h"
#include "tbcad/geom/geom.h"

namespace tbcad::blocks {

// Block-space display lists, shared between the Inserts of one block. An entry is valid for one
// document revision and one combination of insert layer, ByBlock style and tessellation
// tolerance, so a cache must only ever be used with a single Document.
class BlockDisplayCache {
public:
    struct Key {
        BlockId block{};
        LayerId layer{};
        Rgba color{};
        double lineweight = 0;
        const std::vector<double>* pattern = nullptr;
        double tolerance = 0;
        int transparency = 0;          // what ByBlock transparency means inside; -1 = shown opaque
        bool fill = true;              // FILL: solid fills shown (false = outlines only)
        bool qtext = false;            // QTEXT: text shown as boxes
        const void* adjust = nullptr;  // the view's layer overrides (DisplayContext::layerAdjust)
        bool operator<(const Key& o) const {
            return std::tie(block, layer, color.r, color.g, color.b, color.a, lineweight, pattern,
                            tolerance, transparency, fill, qtext,
                            adjust) < std::tie(o.block, o.layer, o.color.r, o.color.g, o.color.b,
                                               o.color.a, o.lineweight, o.pattern, o.tolerance,
                                               o.transparency, o.fill, o.qtext, o.adjust);
        }
    };

    // Drops everything when the revision differs from the one the entries were built for.
    void sync(std::uint64_t revision);
    // Keeps the entries for `revision`: the caller knows the change to it left every block's
    // content and look as it was (only objects outside blocks changed).
    void carryTo(std::uint64_t revision) { revision_ = revision; }
    std::uint64_t revision() const { return revision_; }
    std::shared_ptr<const DisplayList> find(const Key& key) const;
    void put(const Key& key, std::shared_ptr<const DisplayList> list);
    void clear();
    std::size_t size() const { return entries_.size(); }

    // The attribute definitions among a block's entities, in block order (found once per
    // revision: every reference draws its own attribute values from them).
    const std::vector<EntityId>& attdefsOf(const Document& doc, const BlockDef& def);

    // Number of block-space lists built (cache misses) since construction; for tests and tuning.
    std::size_t builds() const { return builds_; }
    void countBuild() { ++builds_; }

private:
    std::map<Key, std::shared_ptr<const DisplayList>> entries_{};
    std::unordered_map<BlockId, std::vector<EntityId>> attdefs_{};
    std::uint64_t revision_ = ~std::uint64_t{0};
    std::size_t builds_ = 0;
};

// While a scope is alive, Insert display generation on this thread uses `cache`. The scene
// builder opens one around a rebuild so that many Inserts of one block tessellate it once.
class BlockDisplayScope {
public:
    explicit BlockDisplayScope(BlockDisplayCache& cache);
    ~BlockDisplayScope();
    BlockDisplayScope(const BlockDisplayScope&) = delete;
    BlockDisplayScope& operator=(const BlockDisplayScope&) = delete;

private:
    BlockDisplayCache* previous_;
};

// Display list of an Insert: the block's entities in block space (ByBlock colour, lineweight and
// linetype resolved from the Insert, layer-0 entities moved onto the Insert's layer), transformed
// by the instance transform. This is what the registered generator calls.
void buildInsertDisplay(const Document& doc, const Entity& insert, const geom::DisplayContext& ctx,
                        DisplayList& out);

// An Insert drawn as a shared block-space display list plus one transform, for renderers that can
// place the same geometry many times without copying it per reference. Only inserts whose look is
// "the block's list moved by transforms, plus the attribute values" qualify: no clipping,
// dynamic parameters or assembly. Needs an active BlockDisplayScope; false means build the full
// display list with buildInsertDisplay instead.
struct InsertInstance {
    std::shared_ptr<const DisplayList> blockSpace{};
    std::vector<Transform2> placements{};  // one per array item (one for a plain reference)
    DisplayList extra{};  // attribute values: drawn per reference, already in drawing space
};
bool buildInsertInstance(const Document& doc, const Entity& insert, const geom::DisplayContext& ctx,
                         InsertInstance& out);

// Registers the Insert display and bounds generators with geom::DisplayRegistry. Safe to call
// more than once; the application calls it at start-up (the command module does it from
// registerCommands).
void registerInsertDisplay();

}  // namespace tbcad::blocks
