#include "panels.h"
#include "tbcad/inquiry/inquiry.h"

namespace tbcad::inquiry {

void registerUi(ui::UiRegistry& registry) {
    registry.addMenuItem("Tools/Inquiry/Distance", "DIST", "inquiry.dist");
    registry.addMenuItem("Tools/Inquiry/Area", "AREA", "inquiry.area");
    registry.addMenuItem("Tools/Inquiry/Angle", "ANGLE", "inquiry.angle");
    registry.addMenuItem("Tools/Inquiry/ID Point", "ID", "inquiry.id");
    registry.addMenuItem("Tools/Inquiry/List", "LIST", "inquiry.list");
    registry.addMenuItem("Tools/Inquiry/Quick Measure", "QUICKMEASURE", "inquiry.quickmeasure");
    registry.addMenuItem("Tools/Inquiry/Calculator", "CAL", "inquiry.cal");
    registry.addMenuItem("Tools/Inquiry/Area Table", "AREATABLE", "inquiry.areatable");
    registry.addMenuItem("Tools/Inquiry/Copy Last Result", "COPYRESULT", "inquiry.copyresult");
    registry.addMenuItem("File/Drawing Utilities/Audit", "AUDIT", "file.audit");
    registry.addMenuItem("File/Drawing Utilities/Recover...", "RECOVER", "file.recover");
    registry.addMenuItem("File/Drawing Utilities/Drawing Recovery Manager...", "DRAWINGRECOVERY",
                         "file.drawingrecovery");
    registry.addMenuItem("File/Drawing Properties...", "DWGPROPS", "file.dwgprops");
    registry.addMenuItem("Format/Units...", "UNITS", "format.units");
#ifdef TBCAD_INQUIRY_WITH_IMGUI
    registerPropsPanels(registry);
#endif
}

}  // namespace tbcad::inquiry
