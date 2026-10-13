#pragma once

#include <cstdint>
#include <set>
#include <string>
#include <vector>

#include "tbcad/core/document.h"

// Geometric and dimensional constraints between LINE, CIRCLE, ARC and POINT entities, solved by
// our own Levenberg-Marquardt solver (docs/constraints.md). The constraint list lives in the
// document header, so it is saved with the drawing and follows undo and redo.
namespace tbcad::constraints {

// Which point of an entity a constraint refers to. Whole = the entity itself.
enum class Part : std::uint8_t { Whole, Start, End, Center };

struct Ref {
    EntityId entity{};
    Part part = Part::Whole;
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
};

struct Constraint {
    std::uint32_t id = 0;
    Kind kind = Kind::Coincident;
    Ref a{}, b{};
    double value = 0;  // Distance, Radius, Angle
};

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

}  // namespace tbcad::constraints
