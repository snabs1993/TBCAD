#pragma once

#include <optional>

#include "tbcad/cmd/host.h"
#include "tbcad/core/core.h"

namespace tbcad::index {
class SpatialIndex;
}

namespace tbcad::drafting {

// What the filters and the typed-input parser can see of the open drawing. The application
// swaps the pointers whenever a different document becomes current; the filters hold a pointer
// to one DraftingEnv that lives as long as the application.
struct DraftingEnv {
    const Document* doc = nullptr;
    const index::SpatialIndex* index = nullptr;
    const cmd::IHost* host = nullptr;

    // Direction the polar / ortho filter last imposed, so that a typed distance goes the way the
    // rubber band points. Valid for the cursor position it was computed at.
    struct Tracking {
        Vec2 base{};
        Vec2 rawCursor{};
        Vec2 direction{};  // unit vector
    };
    std::optional<Tracking> tracking{};

    bool ready() const { return doc && index && host; }
};

}  // namespace tbcad::drafting
