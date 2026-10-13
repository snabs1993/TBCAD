#pragma once

#include <span>
#include <string>
#include <vector>

#include "tbcad/cmd/command.h"
#include "tbcad/core/core.h"

namespace tbcad::transform {

struct ApplyReport {
    std::size_t applied = 0;
    std::size_t failed = 0;
    std::string firstError{};
    std::vector<EntityId> created{};  // copies made (copy == true)
};

// Applies `t` to each entity inside the command's transaction: in place, or as a new copy that
// keeps the source's layer and properties. Entities the transform cannot represent (for example
// non-uniform scale of text) are skipped and counted in `failed`.
ApplyReport applyTransform(cmd::CommandContext& ctx, std::span<const EntityId> ids,
                           const Transform2& t, bool copy);

// Fills `out` with what applyTransform would produce, for the rubber-band preview. Large
// selections (more than `kPreviewEntityLimit`) are previewed as their transformed bounding box.
constexpr std::size_t kPreviewEntityLimit = 400;
void buildPreview(const Document& doc, std::span<const EntityId> ids, const Transform2& t,
                  double pixelSizeMm, DisplayList& out);

}  // namespace tbcad::transform
