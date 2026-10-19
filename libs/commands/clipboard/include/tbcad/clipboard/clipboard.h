#pragma once

#include <map>
#include <optional>
#include <span>
#include <vector>

#include "tbcad/cmd/registry.h"
#include "tbcad/core/core.h"

namespace tbcad::ui {
class UiRegistry;
}

namespace tbcad::clipboard {

// A copy of some entities together with the table rows they refer to, so that it can be pasted
// into the same drawing or another one. Rows are matched by name on paste and created when
// missing.
struct Clip {
    struct BlockCopy {
        BlockDef def{};
        std::vector<Entity> entities{};
    };
    std::vector<Entity> entities{};
    Vec2 base{};  // the point that lands on the insertion point
    std::map<LayerId, Layer> layers{};
    std::map<LevelId, Level> levels{};
    std::map<LinetypeId, Linetype> linetypes{};
    std::map<TextStyleId, TextStyle> textStyles{};
    std::map<DimStyleId, DimStyle> dimStyles{};
    std::map<BlockId, BlockCopy> blocks{};
    // Named plot styles (.stb): the own style name of each copied entity (parallel to `entities`,
    // empty = follows its layer) and of the layers copied with them.
    std::vector<std::string> plotStyles{};
    std::map<LayerId, std::string> layerPlotStyles{};

    bool empty() const { return entities.empty(); }
    // Bounding box of the copied entities (their own extents, without the table rows).
    BBox2 bounds{};
};

// Copies the given entities; ids that no longer exist are skipped. Without `base` the
// lower-left corner of the bounding box is used.
Clip capture(const Document& doc, std::span<const EntityId> ids,
             std::optional<Vec2> base = std::nullopt);

struct PasteOptions {
    Vec2 target{};         // where `clip.base` lands
    bool asBlock = false;  // one Insert of a new block instead of loose entities
    LayerId layer{};       // when set, everything pasted goes on this layer
};

// Adds the clip to the transaction's document. Returns the ids of the new entities (a single
// Insert when asBlock).
Result<std::vector<EntityId>> paste(Transaction& tx, const Clip& clip, const PasteOptions& opts);

// The application-wide clip. Empty until something is copied.
const Clip& current();
void setCurrent(Clip clip);
bool hasClip();

void registerCommands(cmd::Registry& registry);
void registerUi(ui::UiRegistry& ui);

}  // namespace tbcad::clipboard
