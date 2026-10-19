#pragma once

#include <algorithm>

#include "tbcad/render/scene.h"

namespace tbcad::render {

// State carried while one entity is being added.
struct Scene::Cursor {
    EntityId entity{};
    EntityRec* rec = nullptr;
    std::uint32_t drawOrder = 0;
    std::uint32_t level = 0;      // for strokes, fills and points
    std::uint32_t maskLevel = 0;  // for the entity's mask fills
    double builtZoom = 0;
    // Last (tile, batch) used, to skip hash lookups for consecutive segments.
    TileKey tileKey{};
    BatchKey batchKey{};
    Tile* tile = nullptr;
    Batch* batch = nullptr;

    // Appends vertices to the batch for (tileKey, key), extending the entity's range.
    void append(Scene& s, const TileKey& tk, const BatchKey& key, double halfWidthMm,
                std::span<const GpuVertex> v) {
        if (!batch || !(tk == tileKey) || !(key == batchKey)) {
            batch = &s.batchFor(tk, key, halfWidthMm);
            tile = &s.tiles_.at(tk);
            tileKey = tk;
            batchKey = key;
            rec->refs.push_back(s.internRef(tk, key));
        }
        if (batch->ranges.empty() || batch->ranges.back().entity != entity ||
            batch->ranges.back().builtPixelsPerMm != builtZoom ||
            batch->ranges.back().drawOrder != drawOrder) {
            batch->ranges.push_back(
                {entity, static_cast<std::uint32_t>(batch->verts.size()), 0, drawOrder, builtZoom});
            batch->minDrawOrder =
                batch->ranges.size() == 1 ? drawOrder : std::min(batch->minDrawOrder, drawOrder);
        }
        batch->ranges.back().count += static_cast<std::uint32_t>(v.size());
        batch->verts.insert(batch->verts.end(), v.begin(), v.end());
        ++batch->version;
    }
    void expand(const Vec2& p) {
        if (tile)
            tile->bounds.expand(p);
    }
};

}  // namespace tbcad::render
