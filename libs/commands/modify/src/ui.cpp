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
    ui.addMenuItem("Modify/Lengthen", "LENGTHEN", "modify.lengthen", "");
    ui.addMenuItem("Modify/Extrim", "EXTRIM", "modify.extrim", "");
    ui.addMenuItem("Modify/Blend", "BLEND", "modify.blend", "");
    ui.addMenuItem("Modify/Align", "ALIGN", "modify.align", "");
    ui.addMenuItem("Modify/Reverse", "REVERSE", "modify.reverse", "");
    ui.addMenuItem("Edit/Find and Replace", "FIND", "edit.find", "Ctrl+F");
    ui.addMenuItem("Modify/Copy to Layer", "COPYTOLAYER", "modify.copytolayer", "");
    ui.addMenuItem("Format/Rename", "RENAME", "format.rename", "");
    ui.addMenuItem("Modify/Overkill", "OVERKILL", "modify.overkill", "");
    ui.addMenuItem("Modify/Match Properties", "MATCHPROP", "modify.matchprop", "");
    ui.addMenuItem("Modify/Set ByLayer", "SETBYLAYER", "modify.setbylayer", "");
    ui.addMenuItem("Modify/Edit Polyline", "PEDIT", "modify.pedit", "");
    ui.addMenuItem("Modify/Edit Spline", "SPLINEDIT", "modify.splinedit", "");
    ui.addMenuItem("Modify/Scale Text", "SCALETEXT", "modify.scaletext", "");
    ui.addMenuItem("Modify/Justify Text", "JUSTIFYTEXT", "modify.justifytext", "");
    ui.addMenuItem("Modify/Group", "GROUP", "modify.group", "Ctrl+G");
    ui.addMenuItem("Modify/Ungroup", "UNGROUP", "modify.ungroup", "Ctrl+Shift+G");
    ui.addMenuItem("Modify/Draw Order", "DRAWORDER", "modify.draworder", "");
    ui.addMenuItem("Draw/Divide", "DIVIDE", "draw.divide", "");
    ui.addMenuItem("Draw/Measure", "MEASURE", "draw.measure", "");
}

}  // namespace tbcad::modifycmd
