#include "commands.h"
#include "panels.h"
#include "tbcad/levels/levels.h"
#include "tbcad/ui/ui_registry.h"

namespace tbcad::levels {

void registerCommands(cmd::Registry& registry) {
    (void)registry.add(makeLevelPanelCommand());
    (void)registry.add(makeLevelCommand());
    (void)registry.add(makeChangeLevelCommand());
    (void)registry.add(makeLevelUpCommand());
    (void)registry.add(makeLevelDownCommand());
}

void registerUi(ui::UiRegistry& ui) {
    ui.addPanel("levels", "Levels", drawLevelsPanel, /*visibleByDefault=*/true);
    ui.addMenuItem("Format/Levels", "LEVEL", "levels.panel", "Ctrl+Shift+E");
    ui.addMenuItem("Format/Level Up", "LEVELUP", "levels.up", "Ctrl+PageUp");
    ui.addMenuItem("Format/Level Down", "LEVELDOWN", "levels.down", "Ctrl+PageDown");
    ui.addMenuItem("Modify/Move to Level", "CHLEVEL", "levels.move");
}

}  // namespace tbcad::levels
