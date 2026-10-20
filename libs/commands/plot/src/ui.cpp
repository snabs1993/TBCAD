#include "panel.h"
#include "tbcad/plot/plot.h"

namespace tbcad::plot {

void registerUi(ui::UiRegistry& registry) {
    registry.addMenuItem("File/Plot...", "PLOTPANEL", "file.plot",
                         "Ctrl+P");  // the dialog; PLOT is the typed form
    registry.addMenuItem("File/Publish...", "PUBLISH", "file.publish");
    registry.addMenuItem("File/Plot Style Tables...", "PLOTSTYLE", "file.plot_styles");
    registry.addMenuItem("File/Export PDF", "EXPORTPDF", "file.export_pdf");
    registry.addMenuItem("File/Export SVG", "EXPORTSVG", "file.export_svg");
    registry.addMenuItem("File/Export PNG", "EXPORTPNG", "file.export_png");
    registry.addMenuItem("File/Export JPEG", "EXPORTJPG", "file.export_jpg");
    registry.addMenuItem("File/Page Setup Manager...", "PAGESETUPPANEL", "file.page_setup");
    registry.addMenuItem("File/Plot Preview", "PREVIEW", "file.plot_preview");
#ifdef TBCAD_PLOT_WITH_IMGUI
    registerPlotPanel(registry);
#endif
}

}  // namespace tbcad::plot
