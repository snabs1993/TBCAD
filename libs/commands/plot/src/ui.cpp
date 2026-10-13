#include "panel.h"
#include "tbcad/plot/plot.h"

namespace tbcad::plot {

void registerUi(ui::UiRegistry& registry) {
    registry.addMenuItem("File/Plot Preview...", "PLOTPANEL", "file.plot_panel");
    registry.addMenuItem("File/Plot...", "PLOT", "file.plot", "Ctrl+P");
    registry.addMenuItem("File/Plot Style Tables...", "PLOTSTYLE", "file.plot_styles");
    registry.addMenuItem("File/Export PDF", "EXPORTPDF", "file.export_pdf");
    registry.addMenuItem("File/Export SVG", "EXPORTSVG", "file.export_svg");
    registry.addMenuItem("File/Export PNG", "EXPORTPNG", "file.export_png");
    registry.addMenuItem("File/Export JPEG", "EXPORTJPG", "file.export_jpg");
    registry.addMenuItem("File/Page Setup...", "PAGESETUP", "file.page_setup");
#ifdef TBCAD_PLOT_WITH_IMGUI
    registerPlotPanel(registry);
#endif
}

}  // namespace tbcad::plot
