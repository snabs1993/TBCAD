#include "commands.h"
#include "tbcad/text/text.h"
#include "tbcad/textcmd/textcmd.h"
#include "tbcad/ui/ui_registry.h"

namespace tbcad::textcmd {

void registerCommands(cmd::Registry& registry) {
    text::registerDisplay();
    (void)registry.add(makeTextCommand());
    (void)registry.add(makeMTextCommand());
    (void)registry.add(makeTextEditCommand());
    (void)registry.add(makeStyleCommand());
    (void)registry.add(makeStyleManagerCommand());
    (void)registry.add(makeTextStylesPanelCommand());
}

void registerUi(ui::UiRegistry& ui) {
    ui.addMenuItem("Draw/Text/Single-line Text", "TEXT");
    ui.addMenuItem("Draw/Text/Multiline Text", "MTEXT");
    ui.addMenuItem("Modify/Text/Edit Text", "TEXTEDIT");
    ui.addMenuItem("Format/Text Style...", "STYLE");
    ui.addMenuItem("Format/Text Style Manager", "TEXTSTYLES", "format.textstyles");
#ifdef TBCAD_TEXTCMD_IMGUI
    registerEditorPanel(ui);
    registerStylePanel(ui);
#endif
}

}  // namespace tbcad::textcmd
