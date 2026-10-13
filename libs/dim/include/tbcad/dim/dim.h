#pragma once

#include <string>
#include <vector>

#include "tbcad/core/core.h"
#include "tbcad/geom/entity.h"

// Dimensions and leaders for TBCAD (W5-5). Behaviour and the fit rules are in docs/dim.md.
namespace tbcad::dim {

// ---- style and measurement -------------------------------------------------------------------

// The style a dimension or leader uses: its own, else the document's current one, else a built-in
// default (so a damaged file still draws). The reference stays valid while the document does.
const DimStyle& styleOf(const Document& doc, DimStyleId id);

// Measured value computed from the definition points: millimetres for the length kinds, radians
// for the angular kinds. The cached DimensionData::measurement is ignored by the generator.
double measurement(const DimensionData& d);

// The text shown for a dimension: the measurement with the style's units, precision, rounding,
// zero suppression, prefix and suffix (R / Ø for radius / diameter, ° for angles), or the entity's
// text override where "<>" stands for the measurement.
std::string dimensionText(const DimStyle& style, const DimensionData& d);

// Number formatting shared with tests and the style editor. `value` is millimetres, or radians
// when `angular`.
std::string formatNumber(const DimStyle& style, double value, bool angular);

// ---- geometry --------------------------------------------------------------------------------

enum class Role { DimLine, ExtLine, Text };

// What a dimension is made of, before it becomes display items or exploded entities.
struct Layout {
    struct Line {
        std::vector<Vec2> pts{};
        Role role = Role::DimLine;
    };
    struct Arc {
        Vec2 center{};
        double radius = 0;
        double startAngle = 0, endAngle = 0;  // counter-clockwise, radians
        Role role = Role::DimLine;
    };
    struct Arrow {  // a filled polygon (closed arrow, dot)
        std::vector<Vec2> ring{};
    };
    struct Text {
        std::string utf8{};
        Vec2 center{};  // middle of the text box
        double rotation = 0;
        double height = 0;
        bool mtext = false;  // leader annotation: markup, left / right attached to `center`
        bool attachRight = false;
        double width = 0;  // advance width (dimensions only)
        bool valid() const { return !utf8.empty(); }
    };
    std::vector<Line> lines{};
    std::vector<Arc> arcs{};
    std::vector<Arrow> arrows{};
    Text text{};
};

// The arc of an angular dimension, resolved from the definition points. For the two-line kind the
// vertex is the intersection of the lines and the arc passes through `textPos`; for the
// three-point kind defPts = {first, second, vertex, arc point}. `startAngle` and `sweep` give
// the counter-clockwise sector that is measured.
struct AngularGeometry {
    bool ok = false;
    Vec2 vertex{};
    Vec2 dir1{}, dir2{};  // unit vectors from the vertex along the two sides of the sector
    double startAngle = 0, sweep = 0, radius = 0;
};
AngularGeometry angularGeometry(const DimensionData& d);

// `tolerance` is the chord tolerance used for dots (arcs stay arcs).
Layout layoutDimension(const Document& doc, const DimensionData& d, double tolerance = 0.01);
Layout layoutLeader(const Document& doc, const LeaderData& d, double tolerance = 0.01);

// Recomputes what depends on the definition points: `measurement`, and `textPos` unless the user
// placed the text (textPosUser). Used by the commands after creating or editing a dimension.
void refresh(const Document& doc, DimensionData& d);

// Vertices of an arc as a polyline, for display.
std::vector<Vec2> arcPoints(const Layout::Arc& a, double tolerance);

// ---- display, bounds, explode ----------------------------------------------------------------

// Registers the display generators for EntityKind::Dimension and ::Leader with geom. Idempotent.
void registerDisplay();

void buildDimensionDisplay(const Document& doc, const Entity& e, const geom::DisplayContext& ctx,
                           DisplayList& out);

// Explode hook for W5-3's ExplodeRegistry: lines, arcs, solid hatches for the arrowheads, and
// text. The result carries the dimension's layer, linetype and lineweight.
Result<std::vector<Entity>> explodeDimension(const Document& doc, const Entity& e,
                                             double tolerance = 0.01);

}  // namespace tbcad::dim
