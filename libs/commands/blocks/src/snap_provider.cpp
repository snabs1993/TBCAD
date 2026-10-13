#include "commands.h"
#include "tbcad/blocks/tbcad_blocks.h"
#include "tbcad/drafting/snap_geometry.h"

namespace tbcad::block_cmds {

void registerSnapProvider(drafting::SnapGeometryRegistry& registry) {
    registry.set(EntityKind::Insert,
                 [](const Document& doc, const Entity& insert, drafting::SnapGeometry& out) {
                     // The block's content flattened into the Insert's space; each part offers
                     // what a stand-alone entity of its kind would.
                     for (const Entity& part : blocks::snapGeometry(doc, insert))
                         drafting::SnapGeometryRegistry::instance().collect(doc, part, out);
                 });
}

}  // namespace tbcad::block_cmds
