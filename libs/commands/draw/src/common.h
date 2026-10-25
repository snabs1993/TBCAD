#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "tbcad/cmd/cmd.h"
#include "tbcad/geom/geom.h"

namespace tbcad::draw::detail {

inline Entity makeEntity(EntityData d) {
    return Entity{{}, std::move(d)};
}

// "Specify next point" + {"Close","Undo"} -> "Specify next point [Close/Undo]".
std::string promptWith(std::string_view text, const std::vector<std::string>& keywords);

// Appends the display of `e` (current layer, ByLayer properties) to `out`. Invalid geometry
// draws nothing.
void previewEntity(cmd::CommandContext& ctx, Entity e, DisplayList& out);
void previewLine(cmd::CommandContext& ctx, Vec2 a, Vec2 b, DisplayList& out);

// Adds one entity in the command's transaction; reports a failure on the command line.
bool addEntity(cmd::CommandContext& ctx, EntityData data);

// ---- Arc geometry ------------------------------------------------------------------------------

// Signed angle a -> b folded into (-pi, pi].
double signedAngle(double from, double to);
// Arc around `center` starting at `startPoint`, sweeping `sweep` radians (positive = counter
// clockwise, negative = clockwise). Always stored counter-clockwise, as the model requires.
std::optional<ArcData> arcBySweep(Vec2 center, Vec2 startPoint, double sweep);
// Bulge of the arc from a to b whose tangent at a points along `tangent` (radians); empty when
// the chord is (nearly) opposite to the tangent or degenerate.
std::optional<double> bulgeFromTangent(Vec2 a, Vec2 b, double tangent);
// Bulge of the arc from a to b that sweeps `included` radians (signed).
std::optional<double> bulgeFromIncluded(Vec2 a, Vec2 b, double included);
// Direction of the polyline at the end of segment (a -> b with `bulge`), radians.
double endTangent(Vec2 a, Vec2 b, double bulge);
// Centre of the arc a -> b that sweeps `included` radians (signed); empty when degenerate.
std::optional<Vec2> centerFromIncluded(Vec2 a, Vec2 b, double included);

// End of the most recent model-space line or arc, and the direction the drawing continues in.
struct Continuation {
    Vec2 point{};
    double tangent = 0;
    bool fromArc = false;
};
std::optional<Continuation> lastLineOrArc(const Document& doc);

}  // namespace tbcad::draw::detail
