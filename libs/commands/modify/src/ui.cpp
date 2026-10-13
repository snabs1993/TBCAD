#include "tbcad/modifycmd/modifycmd.h"
#include "tbcad/ui/ui_registry.h"

namespace tbcad::modifycmd {

void registerUi(ui::UiRegistry& ui) {
    ui.addMenuItem("Modify/Erase", "ERASE", "modify.erase", "");
    ui.addMenuItem("Modify/Offset", "OFFSET", "modify.offset", "");
    ui.addMenuItem("Modify/Trim", "TRIM", "modify.trim", "");
    ui.addMenuItem("Modify/Extend", "EXTEND", "modify.extend", "");
    ui.addMenuItem("Modify/Fillet", "FILLET", "modify.fillet", "");
    ui.addMenuItem("Modify/Chamfer", "CHAMFER", "modify.chamfer", "");
    ui.addMenuItem("Modify/Break", "BREAK", "modify.break", "");
    ui.addMenuItem("Modify/Break at Point", "BREAKATPOINT", "modify.breakatpoint", "");
    ui.addMenuItem("Modify/Join", "JOIN", "modify.join", "");
}

}  // namespace tbcad::modifycmd
