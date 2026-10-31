#include "panel.h"
#include "tbcad/layout/plotframes.h"
#include "tbcad/plot/plot.h"

namespace tbcad::plot {

namespace {

// Plot frames are drawn while model space is shown, unless "plot.showFrames" is "0".
bool framesShown(const ui::PanelContext& ctx) {
    return ctx.doc && ctx.host && ctx.host->drawingOwner() == BlockId{} &&
           ctx.host->setting("plot.showFrames").value_or("1") != "0";
}

const Rgba kFrameColor{0, 170, 170, 255};

}  // namespace

void registerUi(ui::UiRegistry& registry) {
    registry.addMenuItem("File/Plot...", "PLOTPANEL", "file.plot",
                         "Ctrl+P");  // the dialog; PLOT is the typed form
    registry.addMenuItem("File/Publish...", "PUBLISH", "file.publish");
    registry.addMenuItem("File/Plot Frames...", "PLOTFRAMEPANEL", "file.plot_frames");
    registry.addMenuItem("File/Plot Style Tables...", "PLOTSTYLE", "file.plot_styles");
    registry.addMenuItem("File/Export PDF", "EXPORTPDF", "file.export_pdf");
    registry.addMenuItem("File/Export SVG", "EXPORTSVG", "file.export_svg");
    registry.addMenuItem("File/Export PNG", "EXPORTPNG", "file.export_png");
    registry.addMenuItem("File/Export JPEG", "EXPORTJPG", "file.export_jpg");
    registry.addMenuItem("File/Page Setup Manager...", "PAGESETUPPANEL", "file.page_setup");
    registry.addMenuItem("File/Plot Preview", "PREVIEW", "file.plot_preview");

    // Each plot frame: a dashed outline in model space, with its name, paper and scale above the
    // upper-left corner.
    registry.addOverlayDrawer([](ui::PanelContext& ctx, render::Overlay& overlay) {
        if (!framesShown(ctx))
            return;
        render::OverlayStyle st;
        st.color = kFrameColor;
        st.widthPx = 1.0;
        st.dashPx = 8;
        st.gapPx = 4;
        for (const auto& f : layout::plotFrames(*ctx.doc)) {
            const Vec2 a = f.window.min, b = f.window.max;
            const std::vector<Vec2> pts{a, {b.x, a.y}, b, {a.x, b.y}};
            overlay.polyline(render::Space::World, pts, true, st);
        }
    });
    registry.addLabelProvider([](ui::PanelContext& ctx, std::vector<ui::ViewportLabel>& out) {
        if (!framesShown(ctx))
            return;
        for (const auto& f : layout::plotFrames(*ctx.doc))
            out.push_back({{f.window.min.x, f.window.max.y},
                           f.name + "  " + layout::describeFrame(f),
                           kFrameColor});
    });
#ifdef TBCAD_PLOT_WITH_IMGUI
    registerPlotPanel(registry);
#endif
}

}  // namespace tbcad::plot
