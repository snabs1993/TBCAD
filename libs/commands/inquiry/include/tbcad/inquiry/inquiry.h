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

// Second moments of a closed shape (MASSPROP), all about the centroid, in mm^2 / mm^4.
struct MassProps {
    double area = 0, perimeter = 0;
    Vec2 centroid{};
    double ixx = 0, iyy = 0, ixy = 0;
    double rx = 0, ry = 0;  // radii of gyration
    BBox2 bounds{};
};
// Empty for anything that is not a closed circle, ellipse, closed polyline or closed spline.
std::optional<MassProps> massPropsOfEntity(const Entity& e);

// Angle in [0, pi] between the rays vertex->a and vertex->b.
double angleAtVertex(Vec2 vertex, Vec2 a, Vec2 b);

// Angle in [0, pi] between two lines (taken as infinite). Each ray runs from the lines'
// intersection towards the pick point on that line; for parallel lines the rays follow the
// direction of each line towards its pick (so the result is 0 or pi).
double angleBetweenLines(Vec2 a0, Vec2 a1, Vec2 pickA, Vec2 b0, Vec2 b1, Vec2 pickB);

// Multi-line description of an entity for LIST (lengths in the document's units).
std::vector<std::string> describeEntity(const Document& doc, const Entity& e, Precision precision);

// ---- Quick measure ----------------------------------------------------------------------------

struct Measurement {
    enum class Kind { Length, Radius, Distance };
    Kind kind = Kind::Length;
    double value = 0;  // mm
    Vec2 a{}, b{};     // the two ends of the measured span (for a radius: centre and rim)
    std::string text{};
};

// What QUICKMEASURE shows with the cursor at `cursor`: the length of a line or straight polyline
// segment, the radius of a circle or arc, and the shortest distance between the two objects nearest
// the cursor (omitted when they touch). `nearby` are the objects within the pick aperture.
std::vector<Measurement> quickMeasure(const Document& doc, const std::vector<const Entity*>& nearby,
                                      Vec2 cursor, Precision precision);

// ---- Area table -------------------------------------------------------------------------------

struct AreaTableOptions {
    double textHeight = 2.5;
    Precision precision = 2;
};

// Lines and text of a table (header, one row per closed shape, a total row) whose top-left corner
// is `topLeft`. Shapes that are not closed are skipped.
std::vector<EntityData> buildAreaTable(const Document& doc,
                                       const std::vector<const Entity*>& shapes, Vec2 topLeft,
                                       const AreaTableOptions& options);

// Unit-aware formatting used for all results.
std::string formatLengthIn(const Document& doc, double mm, Precision precision);
std::string formatAreaIn(const Document& doc, double mm2, Precision precision);

}  // namespace tbcad::inquiry
