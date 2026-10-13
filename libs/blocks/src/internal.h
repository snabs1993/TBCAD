#pragma once

#include <string>
#include <vector>

#include "tbcad/blocks/blocks.h"
#include "tbcad/geom/geom.h"

namespace tbcad::blocks::detail {

// A block entity as it appears in an Insert: layer 0 becomes the Insert's layer and ByBlock
// colour, linetype and lineweight become the Insert's own values.
Entity resolveChild(const Entity& child, const Entity& insert);

// Walks an Insert's contents into the space reached by `outer` (which already includes the
// Insert's own instance transform). Nested Inserts are expanded recursively when `expandNested`,
// otherwise they are placed as Inserts when their transform can be represented.
struct Flatten {
    const Document& doc;
    bool expandNested = false;
    bool curvesOnly = false;  // drop kinds without curve or point geometry
    std::vector<Entity> out{};
    std::vector<std::string> messages{};

    void contents(const Entity& insert, const Transform2& outer, int depth);
    void place(Entity child, const Transform2& t, int depth);

private:
    void note(const std::string& m);
};

}  // namespace tbcad::blocks::detail
