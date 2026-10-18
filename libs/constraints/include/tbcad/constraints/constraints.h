#pragma once

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "tbcad/core/document.h"

// Geometric and dimensional constraints between LINE, CIRCLE, ARC, POINT, polyline (straight
// segments), ELLIPSE (centre) and INSERT (position and rotation) entities, solved by
// our own Levenberg-Marquardt solver (docs/constraints.md). The constraint list lives in the
// document header, so it is saved with the drawing and follows undo and redo.
namespace tbcad::constraints {

// Which point of an entity a constraint refers to. Whole = the entity itself. On a polyline,
// Start + `index` is vertex `index` and Whole + `index` is the straight segment from vertex `index`
// to the next one. An insert is a point (Start; index 0 is its insertion point, index k the k-th
// connection point, see insertAnchors) and a direction (Whole: the x axis of the insert through
// that point).
enum class Part : std::uint8_t { Whole, Start, End, Center };

struct Ref {
    EntityId entity{};
    Part part = Part::Whole;
    std::uint32_t index = 0;
    friend bool operator==(const Ref&, const Ref&) = default;
};

enum class Kind : std::uint8_t {
    Coincident,     // a, b: points
    Horizontal,     // a: line (Whole) or point; b: second point when a is a point
    Vertical,       // as Horizontal
    Parallel,       // a, b: lines
    Perpendicular,  // a, b: lines
    Equal,          // a, b: two lines (length) or two circles/arcs (radius)
    Tangent,        // a, b: line and circle/arc, or two circles/arcs
    Fix,            // a: any entity (Whole) or a point; stays where it is
    Distance,       // a, b: points; value in mm
    Radius,         // a: circle or arc; value in mm
    Angle,          // a, b: lines; value in radians, from a's direction to b's
    Concentric,     // a, b: circles, arcs or points; the centres coincide
    PointOnObject,  // a: point; b: line (its infinite extension), circle or arc (full circle)
    Midpoint,       // a: point; b: line; the point sits at the middle of the line
    Collinear,      // a, b: lines on one common line
};

struct Constraint {
    std::uint32_t id = 0;
    Kind kind = Kind::Coincident;
    Ref a{}, b{};
    double value = 0;  // Distance, Radius, Angle
    // Dimensional constraints only: a formula over the named parameters (angles in degrees). When
    // set, `value` follows it each time applyParameters runs.
    std::string expr{};
};

// Connection points of an insert: index 0 is its insertion point, then every POINT object inside
// the block in drawing order (so a pin hole or a slide track end is drawn as a POINT in the block).
// Offsets are from the insertion point, in the insert's own axes (scale applied, not rotated).
std::vector<Vec2> insertAnchors(const Document& doc, const InsertData& in);

std::string_view kindName(Kind k);  // "Coincident", "Horizontal", ...
bool isDimensional(Kind k);

// One-line description for lists: "#3 Parallel line 12 / line 15".
std::string describe(const Document& doc, const Constraint& c);

// Checks that the references fit the kind (entity types, parts, value range).
Result<void> validate(const Document& doc, const Constraint& c);

class ConstraintSet {
public:
    const std::vector<Constraint>& all() const { return items_; }
    bool empty() const { return items_.empty(); }
    // Assigns a fresh id and returns it.
    std::uint32_t add(Constraint c);
    bool remove(std::uint32_t id);
    const Constraint* find(std::uint32_t id) const;
    Constraint* find(std::uint32_t id);
    bool involves(EntityId e) const;
    // Removes every constraint that refers to an entity `exists` rejects. Returns how many.
    template <class Pred>
    std::size_t removeIf(Pred&& missing) {
        std::size_t n = 0;
        for (auto it = items_.begin(); it != items_.end();) {
            if (missing(it->a.entity) || (it->b.entity && missing(it->b.entity))) {
                it = items_.erase(it);
                ++n;
            } else {
                ++it;
            }
        }
        return n;
    }

    // Text form kept in Header::custom["tbcad.constraints"], one constraint per line.
    std::string toText() const;
    static ConstraintSet fromText(const std::string& text);

private:
    std::vector<Constraint> items_{};
    std::uint32_t next_ = 1;
};

inline constexpr const char* kHeaderKey = "tbcad.constraints";

ConstraintSet load(const Document& doc);
// Writes the set into the header (removing the key when it is empty). Part of the transaction.
Result<void> store(Transaction& tx, const ConstraintSet& set);

// ---- parameters -------------------------------------------------------------------------------

// Named values that dimensional constraints can refer to: "Width = 120", "Height = Width * 0.5".
// Names are letters, digits and underscores (not starting with a digit); formulas use the
// libs/expr language (the other parameters, numbers, + - * / ^, min max abs sqrt if(...), pi).
struct Parameter {
    std::string name{};
    std::string expr{};
};

class ParameterSet {
public:
    const std::vector<Parameter>& all() const { return items_; }
    bool empty() const { return items_.empty(); }
    const Parameter* find(const std::string& name) const;
    // Adds or replaces. Fails for a bad name, a formula that does not parse, a name that is
    // reserved (pi, function names) or a formula that would make the parameters depend on
    // themselves.
    Result<void> set(const std::string& name, const std::string& expr);
    bool remove(const std::string& name);
    // Every parameter evaluated (in dependency order); an error names the one that fails.
    Result<std::map<std::string, double>> values() const;
    // A formula evaluated against the parameters.
    Result<double> evaluate(const std::string& expr) const;
    // Names the formula refers to that are not parameters.
    std::vector<std::string> unknownNames(const std::string& expr) const;

    // Header::custom["tbcad.parameters"], one "name<TAB>formula" per line.
    std::string toText() const;
    static ParameterSet fromText(const std::string& text);

private:
    std::vector<Parameter> items_{};
};

inline constexpr const char* kParametersKey = "tbcad.parameters";
ParameterSet loadParameters(const Document& doc);
Result<void> storeParameters(Transaction& tx, const ParameterSet& set);

// ---- solving ---------------------------------------------------------------------------------

struct SolveOptions {
    // Entities whose geometry is kept as it is (the ones the user just edited).
    std::set<EntityId> pinned{};
    int maxIterations = 200;
    double tolerance = 1e-9;  // largest residual that counts as satisfied (mm or unitless)
};

struct SolveResult {
    bool converged = false;
    int iterations = 0;
    double residual = 0;       // largest remaining residual
    int degreesOfFreedom = 0;  // free variables not fixed by the constraints
    std::uint32_t worst = 0;   // constraint with the largest residual when not converged
    std::size_t changedEntities = 0;
};

// Moves the geometry of the constrained entities so that every constraint holds, changing as
// little as possible. Entities are edited through `tx` only when the solve converged.
Result<SolveResult> solve(Transaction& tx, const ConstraintSet& set, const SolveOptions& opt = {});

// The value a dimensional constraint would take for the current geometry (for the default of the
// prompt): distance in mm, radius in mm, angle in radians.
Result<double> measure(const Document& doc, const Constraint& c);

// Re-evaluates every constraint that has a formula against `params`, then solves. On success the
// updated constraint values are stored with `set` (and the geometry follows); otherwise nothing is
// stored and the result says which constraint failed.
Result<SolveResult> applyParameters(Transaction& tx, ConstraintSet& set,
                                    const ParameterSet& params);

// ---- analysis (for the constraint bar and the reports) ---------------------------------------

struct ConstraintStatus {
    std::uint32_t id = 0;
    bool redundant = false;    // the others already imply it (over-constrained, but consistent)
    bool conflicting = false;  // the solve cannot satisfy it together with the others
};

struct EntityStatus {
    EntityId entity{};
    int degreesOfFreedom =
        0;                // how many of its own variables can still move; 0 = fully constrained
    int constraints = 0;  // constraints that name it
};

struct Analysis {
    bool converged = false;
    int degreesOfFreedom = 0;  // of all constrained objects together
    int rows = 0, rank = 0;
    std::uint32_t worst = 0;  // when not converged: the constraint with the largest residual
    std::vector<ConstraintStatus> constraints{};
    std::vector<EntityStatus> entities{};
    bool overConstrained() const;
    bool fullyConstrained() const { return converged && degreesOfFreedom == 0; }
    std::string summary() const;  // "Under-constrained: 5 degrees of freedom", ...
};

// Does not change the document. Redundancy is tested only up to 300 constraints.
Result<Analysis> analyze(const Document& doc, const ConstraintSet& set);

// ---- AUTOCONSTRAIN ----------------------------------------------------------------------------

struct AutoOptions {
    double distance =
        0.01;             // points this close are coincident; lengths and radii this close, equal
    double angle = 0.01;  // radians: lines this close to horizontal, parallel, ... count as such
    bool coincident = true, horizontalVertical = true, parallelPerpendicular = true, equal = true,
         tangent = true;
};

// The constraints the geometry of `ids` already (nearly) satisfies and that `existing` does not
// have yet. Redundant ones are left out: equal lengths chain to the first of a group, and
// parallel/perpendicular pairs of lines that are already horizontal/vertical are skipped.
std::vector<Constraint> autoConstrain(const Document& doc, const std::vector<EntityId>& ids,
                                      const ConstraintSet& existing, const AutoOptions& opt = {});

}  // namespace tbcad::constraints
