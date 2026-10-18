#pragma once

#include <cstddef>
#include <functional>
#include <memory>
#include <vector>

#include "tbcad/core/entities.h"
#include "tbcad/core/math.h"
#include "tbcad/core/result.h"

namespace tbcad::hatch {

// An entity offered to the boundary finder. `entity` must stay alive during the call.
struct Candidate {
    EntityId id{};
    const Entity* entity = nullptr;
};

struct BoundaryOptions {
    // More graph edges than this (after every curve was split at its crossings) aborts the search
    // with BoundaryError::TooManyEdges.
    std::size_t maxEdges = 20000;
    // Gap tolerance (mm). A free end that stops short of another curve (its end or its side) by at
    // most this much is joined to it with a connecting line, which becomes an edge of the loop (it
    // has no source entity). 0 (the default) means ends must meet within 1e-5 mm. Costs one pass
    // over all pieces per free end, so keep it 0 unless the drawing has gaps.
    double gapTolerance = 0;
    // Chord tolerance (mm) used to decide containment and compare areas. Does not change the
    // geometry that is returned.
    double flattenTolerance = 1e-3;
};

struct Boundary {
    // loops[0] is the outer boundary (counter-clockwise, `outer == true`); the rest are loops
    // strictly inside it. Loops at odd nesting depth are holes (`outer == false`), at even depth
    // (2, 4, ...) they are filled again: Normal island style. Every loop is counter-clockwise.
    std::vector<HatchLoop> loops{};
    // Ids of the entities whose edges form the loops: the outer loop's first (each once, in loop
    // order), then those of the islands. Gap connectors have no entity. This is what an
    // associative hatch stores as its source boundary.
    std::vector<EntityId> sources{};
    // Size of the planar graph that was searched, for diagnostics and tests.
    std::size_t graphEdges = 0;
};

enum class BoundaryError {
    NoClosedBoundary,  // no closed region contains the point
    TooManyEdges,      // zoom in: more than BoundaryOptions::maxEdges edges
    NoGeometry,        // no candidate has curve geometry
};

// Message for the command line, e.g. "Too many objects in view; zoom in and try again."
const char* boundaryErrorText(BoundaryError e);

// The planar graph of a set of entities, built once. Use it when several picks are made against
// the same drawing (a hover preview, several HATCH picks): building is the expensive part, a pick
// is cheap. The candidates' entities must stay alive and unchanged while the finder is used.
class BoundaryFinder {
public:
    explicit BoundaryFinder(std::vector<Candidate> candidates, const BoundaryOptions& options = {});
    ~BoundaryFinder();
    BoundaryFinder(BoundaryFinder&&) noexcept;
    BoundaryFinder& operator=(BoundaryFinder&&) noexcept;

    // False when the graph could not be built (too many edges, no geometry): error() says why and
    // every query returns it.
    bool ok() const;
    const Error& error() const;
    std::size_t graphEdges() const;

    // Free ends of lines and arcs (a point where only one edge ends): the usual reason why no
    // closed boundary exists. Empty when the graph could not be built.
    std::vector<Vec2> openEnds() const;

    // As findBoundary / boundaryFromObjects.
    Result<Boundary> find(Vec2 pick) const;
    Result<std::vector<Boundary>> outermost() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Pick-point mode. Splits all candidate curves at their mutual intersections, builds the planar
// graph, walks its faces, and returns the smallest enclosed region that contains `pick` together
// with the loops inside it. Open ends and overlaps are tolerated: a dangling line inside a
// region does not disturb it. Splines that get split are returned as polyline edges.
//
// The Error's message is boundaryErrorText(); its code is Generic for NoClosedBoundary and
// OutOfRange for TooManyEdges.
Result<Boundary> findBoundary(const std::vector<Candidate>& candidates, Vec2 pick,
                              const BoundaryOptions& options = {});

// Candidates whose bounds meet a window (the host's spatial index, or a scan).
using WindowCandidates = std::function<std::vector<Candidate>(const BBox2&)>;

struct NearBoundary {
    Result<Boundary> result;
    // The finder of the last window tried; its open ends explain a failed pick.
    std::shared_ptr<BoundaryFinder> finder;
};

// Pick-point mode for big drawings. Asks for candidates in a small window around `pick` and
// doubles it until the found region lies inside the window; that answer is exact, because
// anything that touches the region also meets the window. A drawing with 50 000 objects then
// costs about as much as the few hundred objects near the pick. `full` is the largest window
// (the view, or the drawing's extents); the last attempt covers all of it and gives the same
// answer (or error) as findBoundary on everything in it.
NearBoundary findBoundaryNear(const WindowCandidates& candidatesIn, const BBox2& full, Vec2 pick,
                              const BoundaryOptions& options = {});

// How islands (closed shapes inside the outer boundary) are treated.
//  Normal: nested regions alternate hole / filled (what findBoundary returns).
//  Outer:  only the first level of islands is cut out; whatever lies inside an island is left
//          alone (not hatched).
//  Ignore: islands are ignored, the whole outer boundary is filled.
enum class IslandStyle { Normal, Outer, Ignore };

// Drops loops according to `style`, and from `sources` the entities that only formed dropped
// loops, so an associative hatch re-derives the same shape. `entityOf` finds a source entity.
void applyIslandStyle(Boundary& b, IslandStyle style,
                      const std::function<const Entity*(EntityId)>& entityOf);

// Select-objects mode. Works on the selected entities only: every region they enclose is found
// the same way as in findBoundary. Each outermost region, together with whatever lies inside it,
// becomes one Boundary (two separate rectangles give two). Closed entities (circles, closed
// polylines, ...) simply become loops, and open entities whose end points meet close up too.
// Boundaries are sorted by the position of their lower-left corner (x, then y). Fails when
// nothing closed was found.
Result<std::vector<Boundary>> boundaryFromObjects(const std::vector<Candidate>& objects,
                                                  const BoundaryOptions& options = {});

}  // namespace tbcad::hatch
