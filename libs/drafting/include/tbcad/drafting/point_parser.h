#pragma once

#include <optional>
#include <string_view>

#include "tbcad/cmd/input.h"
#include "tbcad/drafting/env.h"
#include "tbcad/drafting/ucs.h"

namespace tbcad::drafting {

// Typed point input:
//   x,y        absolute                      10,20   -5.5,3
//   #x,y       absolute (also when dynamic input would make it relative)
//   @dx,dy     relative to the last point     @10,0
//   @d<a       polar, relative to last point  @100<45
//   d<a        polar from the origin          100<45
//   d          direct distance from the base point along `direction` (a unit vector)
//   @          the last point itself
// Lengths take the usual unit suffixes (mm, cm, m, in, ft) and default to the drawing's unit;
// With a UCS, x,y / d<a are in that system (the world result is returned); angles are in
// ctx.angleUnit and counter-clockwise from the +X axis. nullopt = not a point.
std::optional<Vec2> parseTypedPoint(std::string_view text, const cmd::PointParseContext& ctx,
                                    std::optional<Vec2> direction = std::nullopt,
                                    const Ucs& ucs = {});

// Plugs parseTypedPoint into the command runner and supplies the direction for direct distance
// entry: the polar / ortho direction when one is active, else the way the cursor points.
class TypedPointParser final : public cmd::CommandLineParser {
public:
    explicit TypedPointParser(const DraftingEnv* env = nullptr) : env_(env) {}
    std::optional<Vec2> parsePoint(std::string_view text,
                                   const cmd::PointParseContext& ctx) override;

private:
    const DraftingEnv* env_;
};

}  // namespace tbcad::drafting
