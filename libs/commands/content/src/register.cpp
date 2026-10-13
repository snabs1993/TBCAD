#include "commands.h"
#include "tbcad/content_cmds/content_cmds.h"

namespace tbcad::content_cmds {

void registerCommands(cmd::Registry& registry) {
    (void)registry.add(makeContentImportCommand());
    (void)registry.add(makeContentPanelCommand());
}

void registerUi(ui::UiRegistry& ui) {
    ui.addMenuItem("Tools/Content Browser", "CONTENT", "tools.content");
#ifdef TBCAD_CONTENT_IMGUI
    registerContentPanel(ui);
#endif
}

}  // namespace tbcad::content_cmds
