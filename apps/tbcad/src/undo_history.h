#pragma once

#include "tbcad/ui/app_core.h"
#include "tbcad/ui/ui_registry.h"

namespace tbcad::app {

// The "Undo History" panel (Edit > Undo History): every step of the drawing's history by name and
// object count, the current place marked; a click on a row goes back or forward to just after
// that step in one go. Steps ahead of the current place (redo) are greyed.
void registerUndoHistoryPanel(ui::AppCore& core, ui::UiRegistry& registry);

}  // namespace tbcad::app
