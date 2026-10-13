#include "commands.h"

#include <algorithm>
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
    (void)registry.add(makeInsertCommand());
    (void)registry.add(makeExplodeCommand());
    (void)registry.add(makeArrayCommand());
    (void)registry.add(makeArrayRectCommand());
    (void)registry.add(makeArrayPolarCommand());
    (void)registry.add(makeArrayPathCommand());
    (void)registry.add(makeArrayEditCommand());
    (void)registry.add(makeBParamCommand());
    (void)registry.add(makeBActionCommand());
    (void)registry.add(makeDynSetCommand());
    (void)registry.add(makeAttDefCommand());
    (void)registry.add(makeAttEditCommand());
    (void)registry.add(makeAttSyncCommand());
    (void)registry.add(makeXAttachCommand());
    (void)registry.add(makeXReloadCommand());
    (void)registry.add(makeXDetachCommand());
    (void)registry.add(makeXBindCommand());
    (void)registry.add(makeXUnloadCommand());
    (void)registry.add(makeXClipCommand());
    (void)registry.add(makeXListCommand());
    (void)registry.add(makeBlockCountCommand());
}

void registerUi(ui::UiRegistry& ui) {
    registerBlockPanel(ui);
    ui.addMenuItem("Draw/Block/Make Block...", "BLOCK", "block.make", "B");
    ui.addMenuItem("Draw/Block/Insert Block...", "INSERT", "block.insert", "I");
    ui.addMenuItem("Draw/Block/Define Attribute...", "ATTDEF", "block.attdef");
    ui.addMenuItem("Modify/Edit Attributes...", "ATTEDIT", "block.attedit");
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
}

namespace detail {

std::vector<EntityId> inDrawOrder(const Document& doc, const std::vector<EntityId>& ids) {
    std::unordered_set<EntityId> want(ids.begin(), ids.end());
    std::vector<EntityId> out;
    for (EntityId id : doc.modelSpace())
        if (want.count(id))
            out.push_back(id);
    return out;
}

bool editable(const Document& doc, const Entity& e) {
    EffectiveLayer l = doc.effective(e.common.layer);
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
