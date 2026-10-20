#include "commands.h"

#include <algorithm>
#include <cstdio>
#include <unordered_set>

#include "tbcad/blocks/tbcad_blocks.h"
#include "tbcad/hatch/hatch.h"

namespace tbcad::block_cmds {

void registerCommands(cmd::Registry& registry) {
    blocks::registerInsertDisplay();
    blocks::ExplodeRegistry::set(
        EntityKind::Hatch,
        [](const Document& doc, const Entity& e) -> Result<blocks::ExplodeResult> {
            auto r = hatch::explodeHatch(doc, e);
            if (!r)
                return r.error();
            return blocks::ExplodeResult{std::move(r).value(), {}};
        });
    registerSnapProvider(drafting::SnapGeometryRegistry::instance());
    (void)registry.add(makeBlockCommand());
    (void)registry.add(makeETransmitCommand());
    (void)registry.add(makeAttredefCommand());
    (void)registry.add(makeBaseCommand());
    (void)registry.add(makeBEditCommand());
    (void)registry.add(makeBSaveCommand(false));
    (void)registry.add(makeBSaveCommand(true));
    (void)registry.add(makeRefSetCommand());
    (void)registry.add(makeBSaveAsCommand());
    (void)registry.add(makeBvStateCommand());
    (void)registry.add(makeBvHideShowCommand(false));
    (void)registry.add(makeBvHideShowCommand(true));
    (void)registry.add(makeBLookupTableCommand());
    (void)registry.add(makeBTestBlockCommand());
    (void)registry.add(makeBAttOrderCommand());
    (void)registry.add(makeInsertCommand());
    (void)registry.add(makeMinsertCommand());
    (void)registry.add(makeExplodeCommand());
    (void)registry.add(makeXplodeCommand());
    (void)registry.add(makeBurstCommand());
    (void)registry.add(makeNCopyCommand());
    (void)registry.add(makeArrayCommand());
    (void)registry.add(makeArrayRectCommand());
    (void)registry.add(makeArrayPolarCommand());
    (void)registry.add(makeArrayPathCommand());
    (void)registry.add(makeArrayEditCommand());
    (void)registry.add(makeArrayRemoveCommand());
    (void)registry.add(makeBParamCommand());
    (void)registry.add(makeBActionCommand());
    (void)registry.add(makeDynSetCommand());
    (void)registry.add(makeAsmDefCommand());
    (void)registry.add(makeAsmSetCommand());
    (void)registry.add(makeAsmInfoCommand());
    (void)registry.add(makeAsmBomCommand());
    (void)registry.add(makeJoinAsmCommand(true));
    (void)registry.add(makeJoinAsmCommand(false));
    (void)registry.add(makeAsmSetSelectedCommand());
    (void)registry.add(makeAttDefCommand());
    (void)registry.add(makeAttDefLineCommand());
    (void)registry.add(makeAttEditCommand());
    (void)registry.add(makeAttExtCommand());
    (void)registry.add(makeAttInCommand());
    (void)registry.add(makeAttTableCommand());
    (void)registry.add(makeAttSyncCommand());
    (void)registry.add(makeBattManCommand());
    (void)registry.add(makeXAttachCommand());
    (void)registry.add(makePdfAttachCommand());
    (void)registry.add(makeImageAttachCommand());
    (void)registry.add(makeImageClipCommand());
    (void)registry.add(makeImageAdjustCommand());
    (void)registry.add(makeImageFrameCommand());
    (void)registry.add(makeImageTransparencyCommand());
    (void)registry.add(makePdfOsnapCommand());
    (void)registry.add(makeUnderlayFadeCommand());
    (void)registry.add(makeXReloadCommand());
    (void)registry.add(makeXDetachCommand());
    (void)registry.add(makeXBindCommand());
    (void)registry.add(makeXUnloadCommand());
    (void)registry.add(makeXClipCommand());
    (void)registry.add(makeXListCommand());
    (void)registry.add(makeXrefPaletteCommand());
    (void)registry.add(makeXPathCommand());
    (void)registry.add(makeXRelinkCommand());
    (void)registry.add(makeBlockCountCommand());
    (void)registry.add(makeBlockReplaceCommand());
}

void registerUi(ui::UiRegistry& ui) {
    registerBlockPanel(ui);
    registerBlockAuthoringPanel(ui);
    registerXrefPanel(ui);
    ui.addMenuItem("Insert/External References Palette", "EXTERNALREFERENCES", "block.xrefpanel");
    ui.addMenuItem("Draw/Block/Make Block...", "BLOCK", "block.make", "B");
    ui.addMenuItem("Draw/Block/Insert Block...", "INSERT", "block.insert", "I");
    ui.addMenuItem("Draw/Block/Define Attribute...", "ATTDEF", "block.attdef");
    ui.addMenuItem("Modify/Edit Block in Place", "BEDIT", "block.bedit");
    ui.addMenuItem("Modify/Save Block Edit", "BSAVE", "block.bsave");
    ui.addMenuItem("Modify/Close Block Edit", "BCLOSE", "block.bclose");
    ui.addMenuItem("Modify/Block Editor/Save Block As...", "BSAVEAS", "block.bsaveas");
    ui.addMenuItem("Modify/Block Editor/Test Block", "BTESTBLOCK", "block.btestblock");
    ui.addMenuItem("Modify/Block Editor/Visibility States...", "BVSTATE", "block.bvstate");
    ui.addMenuItem("Modify/Block Editor/Hide in Visibility State", "BVHIDE", "block.bvhide");
    ui.addMenuItem("Modify/Block Editor/Show in Visibility State", "BVSHOW", "block.bvshow");
    ui.addMenuItem("Modify/Block Editor/Lookup Table...", "BLOOKUPTABLE", "block.blookuptable");
    ui.addMenuItem("Modify/Block Editor/Attribute Order...", "BATTORDER", "block.battorder");
    ui.addMenuItem("Modify/Edit Attributes...", "ATTEDIT", "block.attedit");
    ui.addMenuItem("Tools/Extract Attributes to CSV...", "ATTEXT", "block.attext");
    ui.addMenuItem("Insert/External Reference...", "XATTACH", "block.xattach");
    ui.addMenuItem("Insert/Reload External References", "XRELOAD", "block.xreload");
    ui.addMenuItem("Modify/Clip Block or Xref...", "XCLIP", "modify.xclip");
    ui.addMenuItem("Tools/Block Count...", "BLOCKCOUNT", "block.count");
    ui.addMenuItem("Modify/Explode", "EXPLODE", "modify.explode", "X");
    ui.addMenuItem("Draw/Block/Dynamic Parameter...", "BPARAM", "block.bparam");
    ui.addMenuItem("Draw/Block/Dynamic Action...", "BACTION", "block.baction");
    ui.addMenuItem("Modify/Set Dynamic Value...", "DYNSET", "modify.dynset");
    ui.addMenuItem("Modify/Array...", "ARRAY", "modify.array");
    ui.addMenuItem("Modify/Array/Rectangular", "ARRAYRECT", "modify.arrayrect");
    ui.addMenuItem("Modify/Array/Polar", "ARRAYPOLAR", "modify.arraypolar");
    ui.addMenuItem("Modify/Array/Path", "ARRAYPATH", "modify.arraypath");
    ui.addMenuItem("Modify/Array/Edit Array...", "ARRAYEDIT", "modify.arrayedit");
    ui.addMenuItem("Modify/Array/Remove Items", "ARRAYREMOVE", "modify.arrayremove");

    // Ctrl+click on an item of an associative array takes that item out (ARRAYREMOVE, which then
    // waits for more items; Enter ends).
    ui.addIdleClickHook(
        [](ui::PanelContext& ctx, Vec2 world, double aperture, bool shift, bool ctrl) {
            if (!ctrl || shift || ctx.commandActive || !ctx.doc || !ctx.runCommand)
                return false;
            if (!detail::arrayItemNear(*ctx.doc, world, aperture))
                return false;
            char buf[96];
            std::snprintf(buf, sizeof buf, "ARRAYREMOVE %.17g,%.17g", world.x, world.y);
            ctx.runCommand(buf);
            return true;
        });
}

namespace detail {

std::vector<EntityId> inDrawOrder(const Document& doc, const std::vector<EntityId>& ids) {
    return doc.inDrawOrder(ids);
}

bool editable(const Document& doc, const Entity& e) {
    EffectiveLayer l = doc.effective(e.common);
    return l.visible() && !l.locked;
}

cmd::EntityFilterFn editableFilter(const Document& doc) {
    return [&doc](const Entity& e) { return !doc.ownerBlock(e.common.id) && editable(doc, e); };
}

BBox2 boundsOf(const Document& doc, const std::vector<Entity>& entities) {
    BBox2 box;
    for (const Entity& e : entities)
        box.merge(geom::bounds(e, doc));
    return box;
}

std::string objects(std::size_t n) {
    return std::to_string(n) + (n == 1 ? " object" : " objects");
}

}  // namespace detail

}  // namespace tbcad::block_cmds
