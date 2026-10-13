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
    if (!registry.find("ERASE"))
        (void)registry.add(makeErase());
}

}  // namespace tbcad::modifycmd
