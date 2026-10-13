#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "tbcad/core/math.h"
#include "tbcad/core/result.h"

namespace tbcad::hatch {

// One family of parallel lines. Line k (k = ..., -1, 0, 1, ...) passes through
// origin + k * (delta.x * u + delta.y * n), where u is the unit vector at `angle` and n = perp(u).
// So `delta.y` is the spacing between neighbouring lines and `delta.x` shifts each next line along
// its own direction (this staggers dashes, as in brickwork).
struct PatternFamily {
    double angle = 0;  // radians, counter-clockwise from +X
    Vec2 origin{};     // mm at pattern scale 1
    Vec2 delta{};      // (along, perpendicular) mm at pattern scale 1
    // Alternating lengths in mm at scale 1: > 0 dash, < 0 gap, 0 dot. Empty = continuous. The
    // cycle starts where the line passes through its own origin point.
    std::vector<double> dashes{};

    // Sum of |dash| lengths; 0 for a continuous line.
    double cycleLength() const;
};

struct Pattern {
    std::string name{};  // upper case
    std::string description{};
    bool solid = false;  // SOLID: no lines, the whole area is filled
    std::vector<PatternFamily> families{};
};

// Parses the TBCAD pattern file format (see assets/patterns/tbcad.patterns for the grammar).
// Unknown keys and malformed `line` entries are errors that name the line number.
Result<std::vector<Pattern>> parsePatterns(std::string_view text);

// The patterns shipped with TBCAD (compiled in from assets/patterns/tbcad.patterns): SOLID, LINE45,
// LINE0, CROSS45, GRID, BRICK, DOTS, SAND, EARTH. Never empty.
const std::vector<Pattern>& builtinPatterns();
// Case-insensitive lookup in the built-in set; null when unknown.
const Pattern* findBuiltinPattern(std::string_view name);
// Patterns added at run time (a user's own pattern file). They shadow built-in ones of the same
// name. Not thread-safe against concurrent display generation: call at start-up or between
// redraws.
class PatternRegistry {
public:
    static void add(Pattern pattern);
    static void clearUser();
    // User patterns first, then the built-in set. Case-insensitive. Null when unknown. The pointer
    // stays valid until the next add() or clearUser().
    static const Pattern* find(std::string_view name);
    // Names of every known pattern: built-in ones first in file order, then user ones.
    static std::vector<std::string> names();
};

// The raw text of the built-in pattern file (for tests and for "export patterns").
std::string_view builtinPatternText();

}  // namespace tbcad::hatch
