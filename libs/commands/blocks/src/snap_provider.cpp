#include <unordered_map>

#include "commands.h"
#include "tbcad/blocks/tbcad_blocks.h"
#include "tbcad/drafting/snap_geometry.h"
#include "tbcad/xref/xref.h"

namespace tbcad::block_cmds {

namespace {

// Flattening a block into the insert's space copies every entity of it, which is far too much to
// do on each mouse move over a big block. The result is kept per insert until the drawing changes
// (the document revision moves on with every commit, undo and redo); the few inserts under the
// cursor are the same from one move to the next.
struct FlatCache {
    std::uint64_t docUid = 0;  // not the address: a new document can land on an old one's
    std::uint64_t revision = 0;
    std::unordered_map<EntityId, drafting::SnapGeometry> entries{};
};

constexpr std::size_t kCacheLimit = 128;

void flatten(const Document& doc, const Entity& insert, drafting::SnapGeometry& out) {
    // The block's content flattened into the Insert's space; each part offers what a stand-alone
    // entity of its kind would.
    for (const Entity& part : blocks::snapGeometry(doc, insert))
        drafting::SnapGeometryRegistry::instance().collect(doc, part, out);
}

// Calls `use` with the flattened geometry of `insert`.
template <class F>
void withFlattened(const Document& doc, const Entity& insert, F&& use) {
    // Only the insert stored in the document is cached; a copy being previewed is not.
    if (doc.find(insert.common.id) != &insert) {
        drafting::SnapGeometry g;
        flatten(doc, insert, g);
        use(g);
        return;
    }
    thread_local FlatCache cache;
    if (cache.docUid != doc.uid() || cache.revision != doc.revision()) {
        cache.entries.clear();
        cache.docUid = doc.uid();
        cache.revision = doc.revision();
    }
    auto it = cache.entries.find(insert.common.id);
    if (it == cache.entries.end()) {
        if (cache.entries.size() >= kCacheLimit)
            cache.entries.clear();
        it = cache.entries.try_emplace(insert.common.id).first;
        flatten(doc, insert, it->second);
    }
    use(it->second);
}

}  // namespace

void registerSnapProvider(drafting::SnapGeometryRegistry& registry) {
    registry.set(EntityKind::Insert,
                 [](const Document& doc, const Entity& insert, drafting::SnapGeometry& out) {
                     // An underlay (PDF page) is only a picture to look at, unless PDFOSNAP is on.
                     if (const auto* data = entityAs<InsertData>(insert);
                         data && xref::isUnderlay(doc, data->block) && !pdfOsnap(doc.header()))
                         return;
                     withFlattened(doc, insert, [&](const drafting::SnapGeometry& g) {
                         auto append = [](auto& dst, const auto& src) {
                             dst.insert(dst.end(), src.begin(), src.end());
                         };
                         append(out.endpoints, g.endpoints);
                         append(out.midpoints, g.midpoints);
                         append(out.centers, g.centers);
                         append(out.quadrants, g.quadrants);
                         append(out.nodes, g.nodes);
                         append(out.insertions, g.insertions);
                         append(out.curves, g.curves);
                     });
                 });
}

}  // namespace tbcad::block_cmds
