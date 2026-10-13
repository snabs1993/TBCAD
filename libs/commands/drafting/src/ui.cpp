#include "module_state.h"
#include "tbcad/draftingcmds/drafting_cmds.h"

namespace tbcad::draftingcmds {

void registerUi(ui::UiRegistry& ui) {
    ModuleState& st = moduleState();

    // Which drawing the filters look at.
    ui.addSessionHook([&st](const ui::SessionInfo& s) {
        st.env.doc = s.doc;
        st.env.index = s.index;
        st.env.host = s.host;
        st.env.tracking.reset();
    });

    // osnap > polar/ortho > grid snap, and typed input (including direct distance).
    auto osnap = std::make_unique<drafting::OsnapFilter>(&st.env);
    st.osnap = osnap.get();
    ui.addPointFilter(drafting::kOsnapOrder, std::move(osnap));
    ui.addPointFilter(drafting::kDynamicUcsOrder,
                      std::make_unique<drafting::DynamicUcsFilter>(&st.env));
    ui.addPointFilter(drafting::kPolarOrder, std::make_unique<drafting::PolarOrthoFilter>(&st.env));
    ui.addPointFilter(drafting::kGridSnapOrder,
                      std::make_unique<drafting::GridSnapFilter>(&st.env));
    ui.setLineParser(&st.parser);

    // Grid and snap markers.
    ui.addOverlayDrawer([&st](ui::PanelContext& pc, render::Overlay& overlay) {
        if (!pc.host || !pc.camera)
            return;
        const auto s = drafting::DraftingSettings::read(*pc.host);
        const bool dark = pc.host->setting("ui.theme").value_or("dark") != "light";
        if (s.grid) {
            drafting::GridStyle gs;
            gs.spacingMm = s.gridSpacing;
            gs.lines = s.gridLines;
            gs.dark = dark;
            if (pc.doc) {  // the grid follows the current UCS
                const auto ucs = drafting::currentUcs(pc.doc, pc.host);
                gs.origin = ucs.origin;
                gs.angle = ucs.angle;
            }
            drafting::buildGrid(overlay, *pc.camera, gs);
        }
        if (pc.feedback && pc.feedback->active && !pc.feedback->marker.empty())
            drafting::drawSnapMarker(overlay, *pc.camera, pc.feedback->markerPos,
                                     pc.feedback->marker, dark);
        (void)st;
    });

    // F-keys (rebindable in keybindings.json). The apostrophe makes them work while a command
    // waits for input.
    ui.addMenuItem("Tools/Drafting Aids/Object Snap", "'TOGGLEOSNAP", "drafting.osnap", "F3");
    ui.addMenuItem("Tools/Drafting Aids/Grid", "'TOGGLEGRID", "drafting.grid", "F7");
    ui.addMenuItem("Tools/Drafting Aids/Ortho", "'TOGGLEORTHO", "drafting.ortho", "F8");
    ui.addMenuItem("Tools/Drafting Aids/Snap", "'TOGGLESNAP", "drafting.snap", "F9");
    ui.addMenuItem("Tools/Drafting Aids/Polar Tracking", "'TOGGLEPOLAR", "drafting.polar", "F10");
    ui.addMenuItem("Tools/Drafting Aids/UCS...", "UCS");
    ui.addMenuItem("Tools/Drafting Aids/Dynamic UCS", "'DYNUCS", "drafting.dynucs");
    ui.addMenuItem("Tools/Drafting Aids/Next Snap Point", "'SNAPCYCLE", "drafting.snapCycle",
                   "Tab");
}

}  // namespace tbcad::draftingcmds
