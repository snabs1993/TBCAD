#include "commands.h"
#include "tbcad/modifycmd/modifycmd.h"

namespace tbcad::modifycmd {

void registerCommands(cmd::Registry& registry) {
    (void)registry.add(makeOffset());
    (void)registry.add(makeTrim());
    (void)registry.add(makeExtend());
    (void)registry.add(makeFillet());
    (void)registry.add(makeChamfer());
    (void)registry.add(makeBreak());
    (void)registry.add(makeBreakAtPoint());
    (void)registry.add(makeJoin());
    (void)registry.add(makeLengthen());
    (void)registry.add(makeReverse());
    (void)registry.add(makeDivide());
    (void)registry.add(makeMeasure());
    (void)registry.add(makeAlign());
    (void)registry.add(makeMatchProp());
    (void)registry.add(makeOverkill());
    (void)registry.add(makePedit());
    (void)registry.add(makeBlend());
    (void)registry.add(makeGroup());
    (void)registry.add(makeUngroup());
    (void)registry.add(makeGroupEdit());
    (void)registry.add(makeDrawOrder());
    (void)registry.add(makeTextToFront());
    (void)registry.add(makeHatchToBack());
    (void)registry.add(makeJustifyText());
    (void)registry.add(makeSplineEdit());
    (void)registry.add(makeExtrim());
    (void)registry.add(makeScaleText());
    (void)registry.add(makeSetByLayer());
    (void)registry.add(makeFind());
    (void)registry.add(makeCopyToLayer());
    if (!registry.find("ERASE"))
        (void)registry.add(makeErase());
    (void)registry.add(makeOops());
}

}  // namespace tbcad::modifycmd
