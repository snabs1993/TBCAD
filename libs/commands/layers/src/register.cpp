#include "commands.h"
#include "panels.h"
#include "tbcad/layers/layers.h"
#include "tbcad/ui/ui_registry.h"

namespace tbcad::layers {

void registerCommands(cmd::Registry& registry) {
    (void)registry.add(makeLayerPanelCommand());
    (void)registry.add(makeLayerCommand());
    (void)registry.add(makeLayerCurrentCommand());
    (void)registry.add(makeActiveOnlyCommand());
    (void)registry.add(makePropCommand());
    (void)registry.add(makeLinetypeCommand());
    (void)registry.add(makeCurrentCommand());
    (void)registry.add(makeLayerStateCommand());
    (void)registry.add(makeLayerFilterCommand());
}

void registerUi(ui::UiRegistry& ui) {
    ui.addPanel("layers", "Layers", drawLayersPanel, /*visibleByDefault=*/true);
    ui.addPanel("properties", "Object Properties", drawPropertiesPanel, /*visibleByDefault=*/true);
    ui.addMenuItem("Format/Active Layer Only", "ACTIVEONLY", "layers.activeOnly", "Ctrl+Shift+A");
    ui.addMenuItem("Format/Layers", "LAYER", "layers.panel", "Ctrl+Shift+L");
    ui.addMenuItem("Format/Load Linetypes", "-LINETYPE Load *", "linetype.loadall");
}

}  // namespace tbcad::layers
