#pragma once

#include <functional>
#include <optional>
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

// AutoCAD's child styles: "<style>$0" (linear and aligned), "$2" (angular and arc length), "$3"
// (diameter), "$4" (radius and jogged), "$6" (ordinate), "$7" (leaders). A dimension of style S
// draws with S's child for its kind when the drawing has one, else with S. -1 for no child.
int childStyleCode(DimKind kind);
std::string childStyleName(const std::string& parent, int code);
// The style a dimension draws with: its style's child for its kind, else the style itself.
const DimStyle& drawStyleOf(const Document& doc, const DimensionData& d);
// The style a leader draws with ("$7" child).
const DimStyle& leaderStyleOf(const Document& doc, DimStyleId id);

// A style of a dimension's own: `base` with `change` made, named "<style>$N" after the style it
// comes from, as AutoCAD's dimension overrides (DIMOVERRIDE, the text grip menu). A derived style
// with the same settings is shared; a change that leaves the settings as they are returns `base`.
// Empty when the style cannot be added.
DimStyleId overrideStyle(Transaction& tx, DimStyleId base,
                         const std::function<void(DimStyle&)>& change);

// Measured value computed from the definition points: millimetres for the length kinds, radians
// for the angular kinds. The cached DimensionData::measurement is ignored by the generator.
double measurement(const DimensionData& d);

// The text shown for a dimension: the measurement with the style's units, precision, rounding,
// zero suppression, prefix and suffix (R / Ø for radius / diameter, ° for angles), or the entity's
// text override where "<>" stands for the measurement.
std::string dimensionText(const DimStyle& style, const DimensionData& d);

// Deviation and limits tolerances are shown stacked, upper over lower, in smaller text (as
// AutoCAD draws them): `markup` is the MTEXT form of the whole text, and the parts are plain text.
// Empty for other tolerance kinds and for an override without "<>".
struct StackedText {
    std::string markup{};
    std::string before{}, up{}, down{}, after{};
};
std::optional<StackedText> stackedDimensionText(const DimStyle& style, const DimensionData& d);

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
        double width = 0;      // advance width (dimensions only)
        bool stacked = false;  // dimension text with a stacked tolerance: markup centred
        // A dimension's text point (DXF group 11): the middle of the text, or the point on the
        // dimension line under text lifted above it (DIMTAD), as AutoCAD writes it.
        Vec2 point{};
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

// The measured arc and the dimension arc of an arc-length dimension: defPts = {centre, start
// point, end point, a point on the dimension arc}. The sector runs counter-clockwise from the
// start point to the end point; the dimension arc is never smaller than the measured arc.
struct ArcSpan {
    bool ok = false;
    Vec2 center{};
    double startAngle = 0, sweep = 0;
    double arcRadius = 0;  // radius of the measured arc
    double dimRadius = 0;  // radius of the dimension arc
};
ArcSpan arcSpan(const DimensionData& d);

// `tolerance` is the chord tolerance used for dots (arcs stay arcs).
// `annoScale` multiplies every size of the style (annotative objects); 1 = as styled.
Layout layoutDimension(const Document& doc, const DimensionData& d, double tolerance = 0.01,
                       double annoScale = 1);
Layout layoutLeader(const Document& doc, const LeaderData& d, double tolerance = 0.01,
                    double annoScale = 1);
// A multileader: its lines with arrowheads, the dogleg and the note. A block as content is not
// part of the Layout; the display and explode add it.
Layout layoutMLeader(const Document& doc, const MLeaderData& d, double tolerance = 0.01,
                     double annoScale = 1);

// Recomputes what depends on the definition points: `measurement`, and `textPos` unless the user
// placed the text (textPosUser). Used by the commands after creating or editing a dimension.
void refresh(const Document& doc, DimensionData& d);

// Text put at `at` by hand (its middle). With DIMTMOVE 0 the dimension line of a linear or
// aligned dimension moves with it, as in AutoCAD, keeping the text's lift (DIMTAD) above it.
void placeText(const Document& doc, DimensionData& d, Vec2 at);

// Vertices of an arc as a polyline, for display.
std::vector<Vec2> arcPoints(const Layout::Arc& a, double tolerance);

// ---- display, bounds, explode ----------------------------------------------------------------

// Registers the display generators for EntityKind::Dimension, ::Leader and ::MLeader with geom.
// Idempotent.
void registerDisplay();

void buildDimensionDisplay(const Document& doc, const Entity& e, const geom::DisplayContext& ctx,
                           DisplayList& out);

// Explode hook for W5-3's ExplodeRegistry: lines, arcs, solid hatches for the arrowheads, and
// text. The result carries the dimension's layer, linetype and lineweight.
Result<std::vector<Entity>> explodeDimension(const Document& doc, const Entity& e,
                                             double tolerance = 0.01);

// A multileader as the older objects other programs know: one LEADER per line (arrowhead and
// line, no text), the note as MText and a block as an Insert. Used when a file format has no
// multileader.
Result<std::vector<Entity>> mleaderAsLeaders(const Document& doc, const Entity& e);

}  // namespace tbcad::dim
