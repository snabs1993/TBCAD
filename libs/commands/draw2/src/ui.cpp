#include "tbcad/draw2/draw2.h"
#include "tbcad/ui/ui_registry.h"

namespace tbcad::draw2 {

void registerUi(ui::UiRegistry& ui) {
    ui.addMenuItem("Draw/Point", "POINT", "draw.point", "");
    ui.addMenuItem("Draw/Donut", "DONUT", "draw.donut", "");
    ui.addMenuItem("Draw/Revision Cloud", "REVCLOUD", "draw.revcloud", "");
    ui.addMenuItem("Draw/Boundary", "BOUNDARY", "draw.boundary", "");
    ui.addMenuItem("Draw/Center Mark", "CENTERMARK", "draw.centermark", "");
    ui.addMenuItem("Draw/Centerline", "CENTERLINE", "draw.centerline", "");
}

}  // namespace tbcad::draw2
