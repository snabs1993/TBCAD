#include "tbcad/inquiry/inquiry.h"

namespace tbcad::inquiry {

void registerUi(ui::UiRegistry& registry) {
    registry.addMenuItem("Tools/Inquiry/Distance", "DIST", "inquiry.dist");
    registry.addMenuItem("Tools/Inquiry/Area", "AREA", "inquiry.area");
    registry.addMenuItem("Tools/Inquiry/Angle", "ANGLE", "inquiry.angle");
    registry.addMenuItem("Tools/Inquiry/ID Point", "ID", "inquiry.id");
    registry.addMenuItem("Tools/Inquiry/List", "LIST", "inquiry.list");
    registry.addMenuItem("Tools/Inquiry/Copy Last Result", "COPYRESULT", "inquiry.copyresult",
                         "Ctrl+Shift+C");
}

}  // namespace tbcad::inquiry
