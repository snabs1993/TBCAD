#include "panel.h"
#include "tbcad/plot/plot.h"

namespace tbcad::plot {

void registerUi(ui::UiRegistry& registry) {
    registry.addMenuItem("File/Plot...", "PLOT", "file.plot", "Ctrl+P");
    registry.addMenuItem("File/Export PDF", "EXPORTPDF", "file.export_pdf");
    registry.addMenuItem("File/Export SVG", "EXPORTSVG", "file.export_svg");
#ifdef TBCAD_PLOT_WITH_IMGUI
    registerPlotPanel(registry);
#endif
}

}  // namespace tbcad::plot
