#pragma once

#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "tbcad/cmd/host.h"
#include "tbcad/cmd/registry.h"
#include "tbcad/core/core.h"

namespace tbcad::transform {

// Grip indices from here up belong to the parameters of a dynamic block Insert: index
// kDynGripBase + n is the handle of parameter n (see blocks::dynHandles).
constexpr int kDynGripBase = 1000;

// One grip of one selected entity (index as in geom::gripPoints).
struct Grip {
    EntityId entity{};
    int index = 0;
    Vec2 pos{};
};

// Grip modes, cycled with Enter / Space while a grip is hot.
enum class GripMode { Stretch, Move, Rotate, Scale, Mirror };
const char* gripModeName(GripMode mode);
GripMode nextGripMode(GripMode mode);

// Setting `grips.limit` (system variable GRIPOBJLIMIT, default 100): above this many selected
// entities no grips are shown; 0 means no limit.
constexpr std::size_t kDefaultGripLimit = 100;
std::size_t gripLimit(const cmd::IHost& host);

// Grips of the given entities, or none when there are more than `limit` of them.
std::vector<Grip> collectGrips(const Document& doc, std::span<const EntityId> ids,
                               std::size_t limit = kDefaultGripLimit);

// The grips of the selection as collectGrips finds them, kept while neither the drawing nor the
// selection nor the limit changes: they are asked for on every mouse move and every frame.
// UI thread only.
const std::vector<Grip>& cachedGrips(const Document& doc, std::span<const EntityId> ids,
                                     std::size_t limit);

// Grip nearest to `p` within `apertureMm`, if any.
const Grip* hitGrip(const std::vector<Grip>& grips, Vec2 p, double apertureMm);

// Entities as they would look after dragging `hot` by `delta` in Stretch mode. Every selected
// grip at the same location moves with it (the shared end of two lines stretches both). Each
// entry is the source id and its new state. `error` receives the first failure, if any.
std::vector<std::pair<EntityId, Entity>> stretchResults(const Document& doc,
                                                        const std::vector<Grip>& grips,
                                                        const Grip& hot, Vec2 delta,
                                                        std::string* error = nullptr);

// The part of `stretchResults` that does not depend on the cursor: which entities have a grip at
// the hot grip's place, and which of their grips. A drag keeps one of these and asks for results
// at every mouse move.
struct HotGrips {
    std::vector<EntityId> order{};
    std::vector<std::vector<int>> indices{};
};
HotGrips hotGripsOf(const std::vector<Grip>& grips, const Grip& hot);
// As stretchResults, for at most `limit` entities (a preview of a huge selection shows the first
// few thousand).
std::vector<std::pair<EntityId, Entity>> stretchResults(const Document& doc, const HotGrips& set,
                                                        const Grip& hot, Vec2 delta,
                                                        std::string* error,
                                                        std::size_t limit);

// Transform for the Move / Rotate / Scale / Mirror modes while the cursor is at `p`.
// Rotate: the hot grip follows the cursor around `base`. Scale: the hot grip's distance from
// `base` becomes the cursor's (1 mm reference when the grip sits on the base). Move: the hot
// grip lands on `p`. Mirror: line from `base` to `p`.
std::optional<Transform2> modeTransform(GripMode mode, Vec2 base, Vec2 hotPos, Vec2 p);

// Position of the grip a running GRIPEDIT edits (drawn red); empty when none. UI thread only.
void setHotGrip(std::optional<Vec2> pos);
std::optional<Vec2> hotGrip();

// The command line the UI runs when the user clicks at `world` with grips showing: `GRIPEDIT x,y`
// (plus `Copy` when `copy`), or empty when no grip of `ids` lies within `apertureMm`.
std::string gripClickCommand(const Document& doc, std::span<const EntityId> ids, Vec2 world,
                             double apertureMm, std::size_t limit, bool copy);

void registerGripCommand(cmd::Registry& registry);

}  // namespace tbcad::transform
