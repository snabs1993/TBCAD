#include "module_state.h"

namespace tbcad::draftingcmds {

ModuleState& moduleState() {
    static ModuleState state;
    return state;
}

}  // namespace tbcad::draftingcmds
