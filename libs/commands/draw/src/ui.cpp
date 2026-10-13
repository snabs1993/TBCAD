#include "tbcad/draw/draw.h"
#include "tbcad/ui/ui_registry.h"

namespace tbcad::draw {

void registerUi(ui::UiRegistry& ui) {
    ui.addMenuItem("Draw/Line", "LINE", "draw.line", "");
    ui.addMenuItem("Draw/Polyline", "POLYLINE", "draw.polyline", "");
    ui.addMenuItem("Draw/Rectangle", "RECTANGLE", "draw.rectangle", "");
    ui.addMenuItem("Draw/Polygon", "POLYGON", "draw.polygon", "");
    ui.addMenuItem("Draw/Circle", "CIRCLE", "draw.circle", "");
    ui.addMenuItem("Draw/Arc", "ARC", "draw.arc", "");
    ui.addMenuItem("Draw/Ellipse", "ELLIPSE", "draw.ellipse", "");
    ui.addMenuItem("Draw/Spline", "SPLINE", "draw.spline", "");
}

}  // namespace tbcad::draw
