#pragma once

#include <vector>

#include "tbcad/blocks/assembly.h"
#include "tbcad/core/core.h"

namespace tbcad::blocks {

// Dynamic blocks: parameters and actions stored on the BlockDef (see DynamicDef) and one value per
// parameter on each Insert. The block's entities are the default state; the actions move, stretch,
// scale, turn or mirror them as the values ask.

// Default value of a parameter: the base-to-end distance, angle 0, no flip, first state.
double dynDefault(const DynParam& p);
// The values an insert uses: its own, clamped to the limits, defaults where it has none.
std::vector<double> dynValues(const BlockDef& def, const InsertData& insert);

// The block's entities in the state `values` describe, in block space (ids kept). Entities hidden
// by the visibility parameter are left out. Entities an action cannot transform stay as they were.
std::vector<Entity> evaluateDynamic(const Document& doc, const BlockDef& def,
                                    const std::vector<double>& values);

// The entities an Insert shows: pointers into the document for an ordinary block, into `storage`
// for a dynamic one (evaluated for `insert`) or when `nested` settings from an assembly apply.
// `insert` may be null (the default state). For an assembly pass the resolved insert
// (resolveAssembly) and its nested settings.
std::vector<const Entity*> blockItems(const Document& doc, const BlockDef& def,
                                      const InsertData* insert, std::vector<Entity>& storage,
                                      const std::vector<NestedSet>* nested = nullptr);

// A grip of a dynamic Insert: the handle of a linear or rotation parameter, in drawing space.
struct DynHandle {
    int param = 0;
    Vec2 world{};
};
std::vector<DynHandle> dynHandles(const Document& doc, const Entity& insert);

// The insert with parameter `param` set to `value` (clamped to its limits). Errors for a block
// that is not dynamic or a parameter out of range.
Result<Entity> dynSet(const Document& doc, const Entity& insert, int param, double value);
// The insert with a linear or rotation parameter dragged so its handle sits at `world`.
// A flip or visibility parameter has a click-only grip: dynDrag toggles the flip, or moves to the
// next visibility state, wherever `world` is.
Result<Entity> dynDrag(const Document& doc, const Entity& insert, int param, Vec2 world);
// True for such a click-only grip (flip, visibility).
bool dynGripIsClick(const Document& doc, const Entity& insert, int param);

// Index of the parameter called `name` (ASCII case-insensitive), or -1.
int dynFind(const DynamicDef& def, std::string_view name);

}  // namespace tbcad::blocks
