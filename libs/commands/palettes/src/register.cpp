#include "commands.h"
#include "tbcad/palettes/palettes.h"

namespace tbcad::palettes {

void registerCommands(cmd::Registry& registry) {
    (void)registry.add(makePalettesPanelCommand());
    (void)registry.add(makePaletteCommand());
}

void registerUi(ui::UiRegistry& ui) {
    ui.addMenuItem("Tools/Tool Palettes", "PALETTES", "tools.palettes");
#ifdef TBCAD_PALETTES_IMGUI
    registerPalettePanel(ui);
#endif
}

}  // namespace tbcad::palettes
