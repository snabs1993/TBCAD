#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "tbcad/cmd/cmd.h"
#include "tbcad/geom/geom.h"

namespace tbcad::draw2::detail {

inline Entity makeEntity(EntityData d) {
    return Entity{{}, std::move(d)};
}

// "Specify next point" + {"Close","Undo"} -> "Specify next point [Close/Undo]".
std::string promptWith(std::string_view text, const std::vector<std::string>& keywords);

void previewEntity(cmd::CommandContext& ctx, Entity e, DisplayList& out);
bool addEntity(cmd::CommandContext& ctx, EntityData data);

// Host settings holding a number; `fallback` when unset or unreadable.
double settingNumber(cmd::CommandContext& ctx, const char* key, double fallback);
void saveSetting(cmd::CommandContext& ctx, const char* key, double value);

// Signed area of a closed ring (counter-clockwise positive).
double ringArea(const std::vector<Vec2>& ring);

}  // namespace tbcad::draw2::detail
