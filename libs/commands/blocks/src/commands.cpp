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
}

void registerUi(ui::UiRegistry& ui) {
    registerBlockPanel(ui);
    ui.addMenuItem("Draw/Block/Make Block...", "BLOCK", "block.make", "B");
    ui.addMenuItem("Draw/Block/Insert Block...", "INSERT", "block.insert", "I");
    ui.addMenuItem("Modify/Explode", "EXPLODE", "modify.explode", "X");
    ui.addMenuItem("Modify/Array...", "ARRAY", "modify.array");
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
