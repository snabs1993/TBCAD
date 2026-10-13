#pragma once

#include <optional>
#include <string>
#include <vector>

#include "tbcad/cmd/registry.h"
#include "tbcad/core/core.h"
#include "tbcad/ui/ui_registry.h"

namespace tbcad::inquiry {

// Called by the generated tbcad::registerAllCommands / registerAllUi (see module.cmake).
void registerCommands(cmd::Registry& registry);
void registerUi(ui::UiRegistry& registry);

// ---- Measuring helpers (pure functions, used by the commands and tested directly) -----------

struct AreaResult {
    double area = 0;       // mm^2, always >= 0
    double perimeter = 0;  // mm; circumference / curve length
};

// Area and perimeter of a closed object: circle, full ellipse, closed or open polyline (open ones
// are closed by a chord for the area), closed spline. Empty for anything else.
std::optional<AreaResult> areaOfEntity(const Entity& e);

// Area and perimeter of the polygon through the points (straight segments, closed by a chord).
AreaResult areaOfPoints(const std::vector<Vec2>& pts);

// Angle in [0, pi] between the rays vertex->a and vertex->b.
double angleAtVertex(Vec2 vertex, Vec2 a, Vec2 b);

// Angle in [0, pi] between two lines (taken as infinite). Each ray runs from the lines'
// intersection towards the pick point on that line; for parallel lines the rays follow the
// direction of each line towards its pick (so the result is 0 or pi).
double angleBetweenLines(Vec2 a0, Vec2 a1, Vec2 pickA, Vec2 b0, Vec2 b1, Vec2 pickB);

// Multi-line description of an entity for LIST (lengths in the document's units).
std::vector<std::string> describeEntity(const Document& doc, const Entity& e, Precision precision);

// Unit-aware formatting used for all results.
std::string formatLengthIn(const Document& doc, double mm, Precision precision);
std::string formatAreaIn(const Document& doc, double mm2, Precision precision);

}  // namespace tbcad::inquiry
