#include "tbcad/collab/undo_guard.h"

#include <map>
#include <set>

#include "tbcad/collab/versions.h"
#include "tbcad/io/delta.h"

namespace tbcad::collab {

namespace {

using Image = std::vector<std::uint8_t>;
using Images = std::map<Key, Image>;  // one encoded image per thing; empty = absent

template <class Row>
void rowImages(Images& out, Space space, const std::vector<io::RowDelta<Row>>& rows,
               std::vector<io::RowDelta<Row>> io::Delta::*member) {
    for (const auto& r : rows) {
        io::Delta one;
        (one.*member).push_back(r);
        out[Key{space, r.id}] = r.after ? io::encodeDelta(one) : Image{};
    }
}

// What the delta says each thing looks like afterwards.
Images imagesOf(const io::Delta& d) {
    Images out;
    for (const io::EntityDelta& e : d.entities) {
        io::Delta one;
        one.entities.push_back(e);
        out[Key{Space::Entity, e.id.value}] = e.after ? io::encodeDelta(one) : Image{};
    }
    rowImages(out, Space::Layer, d.layers, &io::Delta::layers);
    rowImages(out, Space::Level, d.levels, &io::Delta::levels);
    rowImages(out, Space::Linetype, d.linetypes, &io::Delta::linetypes);
    rowImages(out, Space::TextStyle, d.textStyles, &io::Delta::textStyles);
    rowImages(out, Space::DimStyle, d.dimStyles, &io::Delta::dimStyles);
    rowImages(out, Space::Block, d.blocks, &io::Delta::blocks);
    return out;
}

// The delta without the things it erases and without header and draw order.
io::Delta presentOnly(const io::Delta& d) {
    io::Delta p;
    for (const auto& e : d.entities)
        if (e.after)
            p.entities.push_back(e);
    auto keep = [](auto& dst, const auto& src) {
        for (const auto& r : src)
            if (r.after)
                dst.push_back(r);
    };
    keep(p.layers, d.layers);
    keep(p.levels, d.levels);
    keep(p.linetypes, d.linetypes);
    keep(p.textStyles, d.textStyles);
    keep(p.dimStyles, d.dimStyles);
    keep(p.blocks, d.blocks);
    return p;
}

// Empty when the drawing shows `expected` for every thing of the delta that is not in `seen`.
std::string compare(const Document& doc, const io::Delta& expected, std::set<Key>& seen) {
    const Images want = imagesOf(expected);
    Result<ChangeSet> cur = io::materialize(presentOnly(expected), doc);
    if (!cur)
        return "something it changed no longer fits the drawing";
    const Images have = imagesOf(io::deltaOf(cur.value().inverted()));
    for (const auto& [key, image] : want) {
        if (!seen.insert(key).second)
            continue;  // a later step of the same entry decides about this thing
        if (image.empty()) {
            if (exists(doc, key))
                return std::string("the ") + spaceName(key.space) + " it removed exists again";
            continue;
        }
        auto it = have.find(key);
        if (it == have.end() || it->second != image)
            return std::string("the ") + spaceName(key.space) + " was changed by someone else";
    }
    return {};
}

}  // namespace

std::string checkUndoRedo(const Document& doc, std::span<const ChangeSet> sets,
                          Direction direction) {
    std::set<Key> seen;
    const bool undo = direction == Direction::Undo;
    for (std::size_t n = 0; n < sets.size(); ++n) {
        const ChangeSet& cs = undo ? sets[sets.size() - 1 - n] : sets[n];
        // Undo expects the entry's own result (the after images); redo expects the state before it.
        io::Delta expected = undo ? io::deltaOf(cs) : io::deltaOf(cs.inverted());
        if (std::string why = compare(doc, expected, seen); !why.empty())
            return (undo ? "Cannot undo: " : "Cannot redo: ") + why + " since.";
    }
    return {};
}

}  // namespace tbcad::collab
