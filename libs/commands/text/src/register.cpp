#include "commands.h"
#include "tbcad/text/text.h"
#include "tbcad/textcmd/textcmd.h"
#include "tbcad/ui/ui_registry.h"

namespace tbcad::textcmd {

void registerCommands(cmd::Registry& registry) {
    text::registerDisplay();
    registerMTextExplode();
    (void)registry.add(makeTextCommand());
    (void)registry.add(makeMTextCommand());
    (void)registry.add(makeTextEditCommand());
    (void)registry.add(makeStyleCommand());
    (void)registry.add(makeFontCheckCommand());
    (void)registry.add(makeStyleManagerCommand());
    (void)registry.add(makeTextStylesPanelCommand());
    (void)registry.add(makeAnnotativeCommand());
    (void)registry.add(makeAnnoScaleCommand());
    (void)registry.add(makeAnnoNewCommand());
    (void)registry.add(makeFieldCommand());
    (void)registry.add(makeScaleListCommand());
    (void)registry.add(makeObjectScaleCommand());
    (void)registry.add(makeAnnoResetCommand());
    (void)registry.add(makeAnnoUpdateCommand());
    (void)registry.add(makeTxt2MTxtCommand());
    (void)registry.add(makeTCountCommand());
    (void)registry.add(makeSpellCommand());
    (void)registry.add(makeMTextFormatCommand());
    (void)registry.add(makeTxtExpCommand());
    (void)registry.add(makeArcTextCommand());
    (void)registry.add(makeTextAlignCommand());
}

void registerUi(ui::UiRegistry& ui) {
    ui.addMenuItem("Draw/Text/Single-line Text", "TEXT");
    ui.addMenuItem("Draw/Text/Multiline Text", "MTEXT");
    ui.addMenuItem("Modify/Text/Edit Text", "TEXTEDIT");
    ui.addMenuItem("Format/Text Style Manager", "STYLE", "format.textstyles");
    ui.addMenuItem("Draw/Text/Field", "FIELD");
    ui.addMenuItem("Format/Annotative Objects", "ANNOTATIVE");
    ui.addMenuItem("Format/Annotation Scale", "CANNOSCALE");
    ui.addMenuItem("Format/Edit Scale List", "SCALELISTEDIT");
#ifdef TBCAD_TEXTCMD_IMGUI
    registerEditorPanel(ui);
    registerStylePanel(ui);
    registerAnnotationSection(ui);
    registerTextContextTab(ui);
#endif
}

}  // namespace tbcad::textcmd
