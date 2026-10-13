#include "tbcad/draw2/draw2.h"

#include "commands.h"

namespace tbcad::draw2 {

void registerCommands(cmd::Registry& registry) {
    (void)registry.add(detail::makePoint());
    (void)registry.add(detail::makePointType());
    (void)registry.add(detail::makeDonut());
    (void)registry.add(detail::makeRevCloud());
    (void)registry.add(detail::makeWipeout());
    (void)registry.add(detail::makeCenterMark());
    (void)registry.add(detail::makeCenterLine());
    (void)registry.add(detail::makeBoundary());
    (void)registry.add(detail::makeSketch());
    (void)registry.add(detail::makeRegion());
    (void)registry.add(detail::makeUnion());
    (void)registry.add(detail::makeSubtract());
    (void)registry.add(detail::makeIntersect());
}

}  // namespace tbcad::draw2
