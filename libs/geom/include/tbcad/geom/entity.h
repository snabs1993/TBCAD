#pragma once

#include <functional>
#include <optional>
#include <vector>

#include "tbcad/core/display.h"
#include "tbcad/core/document.h"
#include "tbcad/core/entities.h"
#include "tbcad/core/math.h"
#include "tbcad/core/result.h"
#include "tbcad/geom/curve.h"

namespace tbcad::geom {

// ---- Display ---------------------------------------------------------------------------------

struct DisplayContext {
    double pixelSizeMm = 0.1;  // size of one screen pixel in drawing units
    // Maximum chord deviation; 0 means pixelSizeMm * 0.25.
    double chordTolMm = 0;
    double defaultLineweightMm = 0.25;  // what LineWeight::Default draws as
    // Values used where an entity says ByBlock (set by the Insert generator).
    Rgba byBlockColor{255, 255, 255, 255};
    double byBlockLineweightMm = 0.25;
    const std::vector<double>* byBlockPattern = nullptr;
    // Scale for annotative objects (model units per paper unit). 0 = the drawing's current
    // annotation scale; a layout viewport sets its own.
    double annoScale = 0;

    double tolerance() const { return chordTolMm > 0 ? chordTolMm : pixelSizeMm * 0.25; }
};

// Resolves ByLayer / ByBlock colour, lineweight and linetype of an entity into a DisplayStyle.
DisplayStyle resolveStyle(const Document& doc, const Entity& e, const DisplayContext& ctx);

using DisplayFn =
    std::function<void(const Document&, const Entity&, const DisplayContext&, DisplayList&)>;
using BoundsFn = std::function<BBox2(const Document&, const Entity&)>;

// Where later modules (text, dimensions, hatch, blocks) plug in their entity kinds. Register at
// start-up, before any concurrent use. Built-in kinds (Line, Circle, Arc, Polyline, Point,
// Ellipse, Spline, Proxy) cannot be overridden.
class DisplayRegistry {
public:
    // `bounds` may be empty: the bounds are then taken from the generated display list.
    static void set(EntityKind kind, DisplayFn display, BoundsFn bounds = {});
    static void clear(EntityKind kind);
    static bool has(EntityKind kind);
};

// Appends the display items of an entity. Layer visibility is not applied here (the renderer
// does that so toggling a layer needs no rebuild); the style carries the layer id.
void buildDisplay(const Document& doc, const Entity& e, const DisplayContext& ctx,
                  DisplayList& out);

// ---- Bounds ----------------------------------------------------------------------------------

// Exact for the built-in kinds. Other kinds use the registry; empty when none is registered.
BBox2 bounds(const Entity& e, const Document& doc);
// Intrinsic geometry only (no registry): empty for kinds that need the document.
BBox2 bounds(const Entity& e);
// Rays and xlines go on without end, so zoom extents, plot extents and the like leave them out.
inline bool countsForExtents(const Entity& e) {
    return kind(e) != EntityKind::Ray;
}

// ---- Curves ----------------------------------------------------------------------------------

// Curve pieces of an entity with intrinsic curve geometry (Line, Circle, Arc, Polyline, Ellipse,
// Spline). A polyline yields one piece per non-degenerate segment. False for other kinds.
bool curvesOf(const Entity& e, std::vector<CurveRef>& out);

struct EntityIntersection {
    Vec2 p{};
    // Entity parameters: line [0,1]; circle / arc / ellipse the angle (see CurveRef); polyline
    // segmentIndex + local t in [0,1]; spline the knot parameter.
    double tA = 0, tB = 0;
};
// Intersections of two entities with curve geometry. `extend` lets a line / arc / ellipse run on
// to infinity / the full circle / the full ellipse; for an open polyline only its first segment
// (backwards) and last segment (forwards) extend. Unsupported kinds give an error.
Result<void> intersectEntities(const Entity& a, const Entity& b,
                               std::vector<EntityIntersection>& out, bool extendA = false,
                               bool extendB = false);

// Point on the entity for its parameter (same convention as EntityIntersection).
std::optional<Vec2> pointAtParam(const Entity& e, double t);

// ---- Distance and selection --------------------------------------------------------------------

// Distance from p to the entity's geometry (to the outline for closed shapes). Kinds without
// intrinsic curve geometry (text, hatch, dimensions, inserts) need `doc` and a registered
// generator; without it the distance is +infinity.
double distance(const Entity& e, Vec2 p, const Document* doc = nullptr);
std::optional<Vec2> closestPoint(const Entity& e, Vec2 p, const Document* doc = nullptr);
bool hitTest(const Entity& e, Vec2 p, double apertureMm, const Document* doc = nullptr);
// Window selection: the whole entity lies inside the box.
bool insideWindow(const Entity& e, const BBox2& window, const Document* doc = nullptr);
// Crossing selection: the entity touches or lies inside the box.
bool crossesWindow(const Entity& e, const BBox2& window, const Document* doc = nullptr);

// ---- Snap helpers ------------------------------------------------------------------------------

std::vector<Vec2> endpoints(const Entity& e);
std::vector<Vec2> midpoints(const Entity& e);
std::vector<Vec2> centers(const Entity& e);
std::vector<Vec2> quadrants(const Entity& e);

// ---- Measures ----------------------------------------------------------------------------------

// Signed area of a polyline closed by its closing segment (counter-clockwise positive), with
// bulge arcs included.
double signedArea(const PolylineData& pl);
double area(const PolylineData& pl);
double length(const PolylineData& pl);
// Length of the entity's curve (circumference for circles); 0 for kinds without one.
double length(const Entity& e);
// Point and local parameter helpers for polylines (t = segmentIndex + local in [0,1]).
Vec2 polylinePointAt(const PolylineData& pl, double t);

// ---- Transforms
// ----------------------------------------------------------------------------------

// Applies an affine transform. A circle / arc under non-uniform scale becomes an ellipse.
// Errors: non-uniform scale of text, annotations, inserts that cannot represent it, and
// polylines with bulges; a singular transform. A mirror keeps text readable: its position moves
// but its rotation is unchanged (glyphs are not flipped).
Result<Entity> transformed(const Entity& e, const Transform2& t);

// ---- Grips
// ---------------------------------------------------------------------------------------

// Grip points in a fixed order per kind (documented in docs/geom.md).
std::vector<Vec2> gripPoints(const Entity& e);
Result<Entity> moveGrip(const Entity& e, int index, Vec2 to);

}  // namespace tbcad::geom
