#pragma once

#include "tbcad/drafting/drafting.h"

namespace tbcad::draftingcmds {

// The application-wide drafting state shared by the commands and the UI hooks. One per process.
struct ModuleState {
    drafting::DraftingEnv env{};
    drafting::TypedPointParser parser{&env};
    // Owned by the UiRegistry; reset by registerUi. Null before that (headless tests).
    drafting::OsnapFilter* osnap = nullptr;
};

ModuleState& moduleState();

}  // namespace tbcad::draftingcmds
