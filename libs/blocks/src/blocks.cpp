#include "tbcad/blocks/blocks.h"

#include <algorithm>
#include <cmath>
#include <unordered_set>

#include "internal.h"

namespace tbcad::blocks {

Transform2 instanceTransform(const InsertData& ins, Vec2 base) {
    return Transform2::translate(ins.position) * Transform2::rotate(ins.rotation) *
           Transform2::scale(ins.scale.x, ins.scale.y) * Transform2::translate(-base);
}

Transform2 instanceTransform(const Document& doc, const InsertData& ins) {
    const BlockDef* def = doc.block(ins.block);
    return instanceTransform(ins, def ? def->base : Vec2{});
}

Result<void> validateBlockName(const Document& doc, std::string_view name, BlockId ignore) {
    std::size_t b = 0, e = name.size();
    while (b < e && (name[b] == ' ' || name[b] == '\t'))
        ++b;
    while (e > b && (name[e - 1] == ' ' || name[e - 1] == '\t'))
        --e;
    name = name.substr(b, e - b);
    if (name.empty())
        return makeError(ErrorCode::InvalidArgument, "A block needs a name");
    if (name.size() > 255)
        return makeError(ErrorCode::InvalidArgument,
                         "A block name can have at most 255 characters");
    if (name.front() == '*')
        return makeError(ErrorCode::InvalidArgument, "A block name cannot start with *");
    for (char c : name) {
        auto u = static_cast<unsigned char>(c);
        if (u < 0x20 || u == 0x7f ||
            std::string_view("<>/\\\":;?*|,=`").find(c) != std::string_view::npos)
            return makeError(ErrorCode::InvalidArgument,
                             "A block name cannot contain control characters or any of < > / \\ \" "
                             ": ; ? * | , = `");
    }
    if (const BlockDef* other = doc.blockByName(name); other && other->id != ignore)
        return makeError(ErrorCode::Conflict,
                         "A block named \"" + other->name + "\" already exists");
    return {};
}

namespace {

// Blocks directly inserted by `block`.
void directChildren(const Document& doc, BlockId block, std::vector<BlockId>& out) {
    const BlockDef* def = doc.block(block);
    if (!def)
        return;
    for (EntityId id : def->entities)
        if (const Entity* e = doc.find(id))
            if (const auto* ins = entityAs<InsertData>(*e))
                out.push_back(ins->block);
}

}  // namespace

bool references(const Document& doc, BlockId from, BlockId target) {
    std::unordered_set<BlockId> seen;
    std::vector<BlockId> stack{from};
    while (!stack.empty()) {
        BlockId b = stack.back();
        stack.pop_back();
        if (!seen.insert(b).second)
            continue;
        std::vector<BlockId> kids;
        directChildren(doc, b, kids);
        for (BlockId k : kids) {
            if (k == target)
                return true;
            stack.push_back(k);
        }
    }
    return false;
}

bool wouldCreateCycle(const Document& doc, BlockId container, BlockId inserted) {
    return container == inserted || references(doc, inserted, container);
}

std::vector<BlockId> dependentBlocks(const Document& doc, const std::vector<BlockId>& seed) {
    std::unordered_set<BlockId> dirty(seed.begin(), seed.end());
    bool changed = true;
    while (changed) {
        changed = false;
        for (const auto& [id, def] : doc.blocks()) {
            if (dirty.count(id))
                continue;
            std::vector<BlockId> kids;
            directChildren(doc, id, kids);
            if (std::any_of(kids.begin(), kids.end(), [&](BlockId k) { return dirty.count(k); })) {
                dirty.insert(id);
                changed = true;
            }
        }
    }
    std::vector<BlockId> r(dirty.begin(), dirty.end());
    std::sort(r.begin(), r.end());
    return r;
}

namespace {

BBox2 contentBounds(const Document& doc, const BlockDef& def, const Transform2& t, int depth) {
    BBox2 box;
    if (depth > kMaxNesting)
        return box;
    for (EntityId id : def.entities) {
        const Entity* e = doc.find(id);
        if (!e || !e->common.visible)
            continue;
        if (const auto* ins = entityAs<InsertData>(*e)) {
            if (const BlockDef* inner = doc.block(ins->block))
                box.merge(contentBounds(doc, *inner, t * instanceTransform(*ins, inner->base),
                                        depth + 1));
            continue;
        }
        auto moved = geom::transformed(*e, t);
        if (moved) {
            box.merge(geom::bounds(moved.value(), doc));
        } else {
            box.merge(t.applyToBox(geom::bounds(*e, doc)));
        }
    }
    return box;
}

}  // namespace

BBox2 insertBounds(const Document& doc, const Entity& insert) {
    const auto* ins = entityAs<InsertData>(insert);
    if (!ins)
        return {};
    const BlockDef* def = doc.block(ins->block);
    if (!def)
        return {};
    return contentBounds(doc, *def, instanceTransform(*ins, def->base), 0);
}

BBox2 blockBounds(const Document& doc, BlockId block) {
    const BlockDef* def = doc.block(block);
    return def ? contentBounds(doc, *def, Transform2::identity(), 0) : BBox2{};
}

std::vector<Entity> snapGeometry(const Document& doc, const Entity& insert) {
    if (!entityAs<InsertData>(insert))
        return {};
    detail::Flatten f{doc};
    f.expandNested = true;
    f.curvesOnly = true;
    const auto* ins = entityAs<InsertData>(insert);
    const BlockDef* def = doc.block(ins->block);
    if (!def)
        return {};
    f.contents(insert, instanceTransform(*ins, def->base), 0);
    return std::move(f.out);
}

}  // namespace tbcad::blocks
