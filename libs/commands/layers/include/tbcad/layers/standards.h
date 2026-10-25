#pragma once

#include <string>
#include <vector>

#include "tbcad/core/core.h"

namespace tbcad::layers {

// CHECKSTANDARDS: the layers, linetypes, text styles and dimension styles of a drawing compared
// with those of a standards drawing (any .tbcad or .dxf, used as AutoCAD uses a .dws).
enum class StandardsKind { Layer, Linetype, TextStyle, DimStyle };
// Rename: the name differs only in case. Replace: the row is not in the standard; what uses it
// moves to `target` and the row goes. Update: the row has the standard's properties set.
enum class StandardsFix { Rename, Replace, Update };

struct StandardsViolation {
    StandardsKind kind = StandardsKind::Layer;
    StandardsFix fix = StandardsFix::Update;
    std::string name{};     // the row in the drawing (a layer's full path)
    std::string target{};   // Rename and Replace: the row to use
    std::string problem{};  // what is wrong, in words
    // "L:name", "Y:name", "T:name" or "D:name": what the Fix button hands to _FIXSTANDARDS.
    std::string key() const;
};

// Layer 0, xref layers and the built-in rows are never reported.
std::vector<StandardsViolation> checkStandards(const Document& drawing, const Document& standards);

struct StandardsResult {
    int fixed = 0;
    std::vector<std::string> failed{};  // "name: why"
};
// Fixes the given violations (as checkStandards found them) in one transaction.
StandardsResult fixStandards(const Document& drawing, const Document& standards, Transaction& tx,
                             const std::vector<StandardsViolation>& violations);

// The verb on a Fix button: "Rename to X", "Replace with X" or "Set to the standard".
std::string fixLabel(const StandardsViolation& v);

}  // namespace tbcad::layers
