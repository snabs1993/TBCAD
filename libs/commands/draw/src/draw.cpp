#include "tbcad/draw/draw.h"

#include "draw_commands.h"

namespace tbcad::draw {

void registerCommands(cmd::Registry& registry) {
    (void)registry.add(detail::makeLine());
    (void)registry.add(detail::makePolyline());
    (void)registry.add(detail::makeCircle());
    (void)registry.add(detail::makeArc());
    (void)registry.add(detail::makeRectangle());
    (void)registry.add(detail::makePolygon());
    (void)registry.add(detail::makeEllipse());
    (void)registry.add(detail::makeSpline());
}

}  // namespace tbcad::draw
