#pragma once

#include <functional>
#include <optional>
#include <span>
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
    int byBlockTransparency = 0;  // percent
    // Off: everything is drawn opaque (the TPY toggle).
    bool showTransparency = true;
    // Off (FILL): solid fills are drawn as their outlines; wipeout masks stay.
    bool fillMode = true;
    // QTEXT: text and multiline text are drawn as a box around where they lie.
    bool quickText = false;
    // ATTDISP: 0 attributes as defined, 1 all shown, 2 all hidden.
    int attDisplay = 0;
    // Scale for annotative objects (model units per paper unit). 0 = the drawing's current
    // annotation scale; a layout viewport sets its own.
    double annoScale = 0;
    // True for the objects of a layout sheet itself (not the model shown in a viewport): their
    // linetypes are not scaled by the model-space annotation scale (MSLTSCALE).
    bool paperSpace = false;

    double tolerance() const { return chordTolMm > 0 ? chordTolMm : pixelSizeMm * 0.25; }
};

// Resolves ByLayer / ByBlock colour, lineweight and linetype of an entity into a DisplayStyle.
DisplayStyle resolveStyle(const Document& doc, const Entity& e, const DisplayContext& ctx);
// The transparency (percent, 0 to 90) the entity draws with after ByLayer / ByBlock; 0 when the
// context shows everything opaque.
int resolveTransparency(const Document& doc, const Entity& e, const DisplayContext& ctx);

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

// SELECTIONANNODISPLAY: what a selected annotative object looks like at each of its scales other
// than the one it is drawn at, so the user sees every representation. Symbols (inserts) are sized
// for their first scale and have none. Empty when the variable is off.
struct AnnotationGhost {
    EntityId id{};
    double scale = 1;  // N of 1:N
    DisplayList display{};
};
std::vector<AnnotationGhost> annotationGhosts(const Document& doc, std::span<const EntityId> ids,
                                              const DisplayContext& ctx);

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

// The curve of the entity as points in the direction of travel (polyline arcs with a negative
// bulge run clockwise; circles, arcs and ellipses run counter-clockwise). Circular arcs are sampled
// until the chord deviates less than `tol` - without the 4096-segment limit of flatten() - other
// curves are flattened. A closed curve does not repeat its first point. False for kinds without a
// curve.
bool outlinePoints(const Entity& e, double tol, std::vector<Vec2>& out);

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
// The index of a dimension's text grip (its last grip).
int dimTextGrip(const DimensionData& d);

// After the parameters of an associative array were changed from `from` to `to`: the removed and
// replaced items of `from` keep their row and column (rectangular) or their index (polar, path);
// those no longer in the array are dropped.
void carryArrayOverrides(const ArrayInfo& from, ArrayInfo& to);

}  // namespace tbcad::geom
