#pragma once

#include <optional>

#include "tbcad/cmd/host.h"
#include "tbcad/core/core.h"
#include "tbcad/drafting/ucs.h"

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

    // Set by the dynamic UCS filter while the cursor is on a straight edge: origin at the start of
    // the edge, X axis along it. Ortho, polar and grid snap use it instead of the current UCS.
    // Mutable because the filters that read it hold the environment as const.
    mutable std::optional<Ucs> dynamicUcs{};

    // The system the point filters work in right now.
    Ucs effectiveUcs() const;

    bool ready() const { return doc && index && host; }
};

}  // namespace tbcad::drafting
