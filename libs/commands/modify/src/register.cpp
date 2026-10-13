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
    if (!registry.find("ERASE"))
        (void)registry.add(makeErase());
}

}  // namespace tbcad::modifycmd
