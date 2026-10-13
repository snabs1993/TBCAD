#pragma once

#include <string_view>
#include <vector>

#include "tbcad/core/core.h"

namespace tbcad::blocks {

// Maximum nesting followed when drawing, measuring or exploding inserts. Definitions can never
// form a cycle through the commands, but a file written by something else might.
inline constexpr int kMaxNesting = 32;

// Transform from block space to the space the Insert lives in:
// translate(position) * rotate(rotation) * scale(sx, sy) * translate(-base).
Transform2 instanceTransform(const InsertData& insert, Vec2 blockBase);
// Same, looking the base point up in the document (identity base when the block is missing).
Transform2 instanceTransform(const Document& doc, const InsertData& insert);

// Block space to the Insert's space for every item of the Insert: one transform for a plain
// Insert, one per item for an associative array (see ArrayInfo; item transforms act in block
// space, before instanceTransform).
std::vector<Transform2> instanceTransforms(const InsertData& insert, Vec2 blockBase);

// ---- Definitions
// ---------------------------------------------------------------------------------

// Empty on success. A block name must be non-empty after trimming, at most 255 bytes, free of
// control characters and of < > / \ " : ; ? * | , = ` and must not start with '*' (anonymous).
// Names compare ASCII case-insensitively. `ignore` lets a block keep its own name.
Result<void> validateBlockName(const Document& doc, std::string_view name, BlockId ignore = {});

// True when block `from` contains an Insert of `target`, directly or through nested blocks.
bool references(const Document& doc, BlockId from, BlockId target);
// True when putting an Insert of `inserted` into the definition `container` would make a cycle
// (`container` inserted into itself, directly or through nesting).
bool wouldCreateCycle(const Document& doc, BlockId container, BlockId inserted);

// Ids of the blocks that contain, directly or through nesting, an Insert of any block in `seed`
// (the seeds themselves are included).
std::vector<BlockId> dependentBlocks(const Document& doc, const std::vector<BlockId>& seed);

// ---- Geometry
// ------------------------------------------------------------------------------------

// Bounds of an Insert in the space it lives in (nested inserts included). Empty for a missing or
// empty block.
BBox2 insertBounds(const Document& doc, const Entity& insert);
// Bounds of the block definition in block space.
BBox2 blockBounds(const Document& doc, BlockId block);

// The geometry an Insert is made of, flattened through nested inserts and expressed in the space
// the Insert lives in, for object snap and picking. Only entities with curve or point geometry
// are returned (inserts are expanded, text and the like are left out). Properties are resolved
// the same way as when exploding. Empty for a missing block.
//
// Intended as the Insert entry of the SnapGeometryProvider hook of the drafting aids.
std::vector<Entity> snapGeometry(const Document& doc, const Entity& insert);

}  // namespace tbcad::blocks
