#include "commands.h"
#include "tbcad/blocks/tbcad_blocks.h"
#include "tbcad/drafting/snap_geometry.h"
#include "tbcad/xref/xref.h"

namespace tbcad::block_cmds {

void registerSnapProvider(drafting::SnapGeometryRegistry& registry) {
    registry.set(EntityKind::Insert,
                 [](const Document& doc, const Entity& insert, drafting::SnapGeometry& out) {
                     // An underlay (PDF page) is only a picture to look at: no snapping to it.
                     if (const auto* data = entityAs<InsertData>(insert);
                         data && xref::isUnderlay(doc, data->block))
                         return;
                     // The block's content flattened into the Insert's space; each part offers
                     // what a stand-alone entity of its kind would.
                     for (const Entity& part : blocks::snapGeometry(doc, insert))
                         drafting::SnapGeometryRegistry::instance().collect(doc, part, out);
                 });
}

}  // namespace tbcad::block_cmds
