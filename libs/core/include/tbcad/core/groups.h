#pragma once

#include <span>
#include <string>
#include <vector>

#include "tbcad/core/document.h"

namespace tbcad {

// Named sets of model-space entities that are selected together (GROUP / UNGROUP). The sets are
// kept in Header::custom["tbcad.groups"], so they are saved with the drawing and follow undo.
struct Group {
    std::string name{};
    std::vector<EntityId> members{};
    bool selectable = true;  // false: picking a member selects only that member
};

// All groups of the document. Members that no longer exist are dropped; groups left with fewer
// than one member are omitted.
std::vector<Group> readGroups(const Document& doc);
// Stores the groups in the transaction's document.
Result<void> writeGroups(Transaction& tx, const std::vector<Group>& groups);

// PICKSTYLE in Header::custom: 0 no group selection, 1 group selection (default), 2 associative
// hatch selection (a picked hatch brings its boundary objects), 3 both.
inline constexpr const char* kPickStyleKey = "tbcad.pickstyle";
int pickStyle(const Header& h);

// HPORIGIN in Header::custom: "x,y", the pattern origin every new hatch starts with.
inline constexpr const char* kHpOriginKey = "tbcad.hporigin";
Vec2 hatchOrigin(const Header& h);

// `ids` plus every member of each selectable group that contains one of them (first occurrence
// order, no duplicates), and the boundaries of associative hatches, as PICKSTYLE says. Cheap
// when the document has no groups.
std::vector<EntityId> expandGroups(const Document& doc, std::span<const EntityId> ids);

}  // namespace tbcad
