#include "tbcad/blocks/blocks.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <tuple>
#include <unordered_map>
#include <unordered_set>

#include "internal.h"
#include "tbcad/blocks/array.h"
#include "tbcad/blocks/assembly.h"
#include "tbcad/blocks/attributes.h"
#include "tbcad/blocks/dynamic.h"

namespace tbcad::blocks {

Transform2 instanceTransform(const InsertData& ins, Vec2 base) {
    return Transform2::translate(ins.position) * Transform2::rotate(ins.rotation) *
           Transform2::scale(ins.scale.x, ins.scale.y) * Transform2::translate(-base);
}

Transform2 instanceTransform(const Document& doc, const InsertData& ins) {
    const BlockDef* def = doc.block(ins.block);
    return instanceTransform(ins, def ? def->base : Vec2{});
}

std::vector<Transform2> instanceTransforms(const InsertData& ins, Vec2 base) {
    Transform2 inst = instanceTransform(ins, base);
    if (!ins.array)
        return {inst};
    std::vector<Transform2> out = arrayItemTransforms(*ins.array);
    const ArrayInfo& a = *ins.array;
    if (a.removed.empty() && a.swaps.empty()) {
        for (Transform2& t : out)
            t = inst * t;
        return out;
    }
    std::vector<Transform2> shown;
    shown.reserve(out.size());
    for (std::size_t i = 0; i < out.size(); ++i)
        if (arrayItemShowsSource(a, static_cast<int>(i)))
            shown.push_back(inst * out[i]);
    return shown;
}

std::vector<SwappedItem> swappedItems(const Document& doc, const InsertData& ins) {
    std::vector<SwappedItem> out;
    if (!ins.array || ins.array->swaps.empty())
        return out;
    const BlockDef* def = doc.block(ins.block);
    const Transform2 inst = instanceTransform(ins, def ? def->base : Vec2{});
    const std::vector<Transform2> items = arrayItemTransforms(*ins.array);
    for (const ArrayItemSwap& sw : ins.array->swaps) {
        const BlockDef* other = doc.block(sw.block);
        if (!other || sw.item < 0 || static_cast<std::size_t>(sw.item) >= items.size())
            continue;
        out.push_back({sw.item, sw.block,
                       inst * items[static_cast<std::size_t>(sw.item)] *
                           Transform2::translate(sw.at - other->base)});
    }
    return out;
}

Vec2 arrayItemBase(const Document& doc, const InsertData& ins) {
    const BBox2 box = blockBounds(doc, ins.block);
    if (box.empty()) {
        const BlockDef* def = doc.block(ins.block);
        return def ? def->base : Vec2{};
    }
    return box.center();
}

std::optional<int> arrayItemAt(const Document& doc, const InsertData& ins, Vec2 p, double tol) {
    if (!ins.array)
        return std::nullopt;
    const BlockDef* def = doc.block(ins.block);
    const Transform2 inst = instanceTransform(ins, def ? def->base : Vec2{});
    const BBox2 source = blockBounds(doc, ins.block);
    std::optional<int> best;
    double bestGap = 0, bestCentre = 0;
    auto consider = [&](int item, const BBox2& box) {
        if (box.empty())
            return;
        const double dx = std::max({box.min.x - p.x, 0.0, p.x - box.max.x});
        const double dy = std::max({box.min.y - p.y, 0.0, p.y - box.max.y});
        const double gap = std::hypot(dx, dy);
        if (gap > tol)
            return;
        const double centre = distance(p, box.center());
        if (!best || gap < bestGap - 1e-12 || (gap <= bestGap + 1e-12 && centre < bestCentre)) {
            best = item;
            bestGap = gap;
            bestCentre = centre;
        }
    };
    const std::vector<Transform2> items = arrayItemTransforms(*ins.array);
    for (std::size_t i = 0; i < items.size(); ++i)
        if (arrayItemShowsSource(*ins.array, static_cast<int>(i)))
            consider(static_cast<int>(i), (inst * items[i]).applyToBox(source));
    for (const SwappedItem& sw : swappedItems(doc, ins))
        consider(sw.item, sw.t.applyToBox(blockBounds(doc, sw.block)));
    return best;
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
// TBCAD_INDEX_PROFILE: an object inside a block that takes long to measure, on stderr.
bool profileBlocks() {
    static const bool on = [] {
        const char* v = std::getenv("TBCAD_INDEX_PROFILE");
        return v && *v && *v != '0';
    }();
    return on;
}

// Blocks directly inserted by `block`.
void directChildren(const Document& doc, BlockId block, std::vector<BlockId>& out) {
    const BlockDef* def = doc.block(block);
    if (!def)
        return;
    for (EntityId id : def->entities)
        if (const Entity* e = doc.find(id))
            if (const auto* ins = entityAs<InsertData>(*e)) {
                out.push_back(ins->block);
                if (ins->array)
                    for (const ArrayItemSwap& sw : ins->array->swaps)
                        out.push_back(sw.block);
            }
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

// `box` limited to the clipping boundary of `ins` seen through `t` (block space of the insert's
// block to the space `box` is in).
BBox2 limitToClip(BBox2 box, const InsertData& ins, const Transform2& t) {
    if (!ins.clipOn || ins.clipInvert || ins.clip.size() < 3 || box.empty())
        return box;
    BBox2 c;
    for (const Vec2& p : ins.clip)
        c.expand(t.apply(p));
    BBox2 out;
    out.min = {std::max(box.min.x, c.min.x), std::max(box.min.y, c.min.y)};
    out.max = {std::min(box.max.x, c.max.x), std::min(box.max.y, c.max.y)};
    if (out.min.x > out.max.x || out.min.y > out.max.y)
        return {};
    return out;
}

// What measuring has learned about blocks. A block with no attributes, dynamic parameters or
// assembly looks the same through every reference, so its bounds are worked out once (in its own
// space) and moved into place for each further use. Without this a block nested five deep with
// four references per level was measured 4^4 times over for every reference to it. The memo lives
// for one call, or for a BoundsMemoScope (many measurements while the drawing does not change).
struct Local {
    bool plain = false;
    bool attdefs = false;    // has attribute definitions of its own (not counted in `box`)
    BBox2 box{};             // everything but its own attribute definitions, in block space
    std::size_t leaves = 0;  // drawn objects below it, counting nested ones (capped)
};
struct Memo {
    std::unordered_map<BlockId, Local> blocks;
    // Exact boxes of plain blocks seen turned (or slanted), by block and the transform without
    // its move: every item of an array, every copy at the same angle, shares one.
    std::map<std::tuple<std::uint64_t, double, double, double, double>, BBox2> turned;
};

thread_local Memo* g_memo = nullptr;

constexpr std::size_t kLooseAbove = 512;  // leaves; see contentBounds

bool axisAligned(const Transform2& t) {
    const double eps =
        1e-12 * (std::abs(t.m00) + std::abs(t.m01) + std::abs(t.m10) + std::abs(t.m11));
    return (std::abs(t.m01) <= eps && std::abs(t.m10) <= eps) ||
           (std::abs(t.m00) <= eps && std::abs(t.m11) <= eps);
}

BBox2 contentBounds(const Document& doc, const BlockDef& def, const Transform2& t, int depth,
                    const InsertData* owner, Memo& memo, bool bypass = false);

// Fills the memo entry of `def` (and of the blocks below it).
Local localOf(const Document& doc, const BlockDef& def, Memo& memo, int depth) {
    if (auto it = memo.blocks.find(def.id); it != memo.blocks.end())
        return it->second;
    Local loc;
    if (depth > kMaxNesting) {
        // Nothing this deep is drawn: measured as empty, so that a chain of blocks that each
        // hold the next one several times is not walked through every path.
        loc.plain = true;
        return loc;
    }
    memo.blocks[def.id] = {};  // (a block that contains itself stays "not plain")
    if (def.assembly || def.dynamic)
        return loc;
    bool plain = true;
    std::size_t leaves = 0;
    for (EntityId id : def.entities) {
        const Entity* e = doc.find(id);
        if (!e)
            continue;
        if (const auto* td = entityAs<TextData>(*e); td && td->attdef) {
            loc.attdefs = true;  // what they show depends on the reference: measured per use
            continue;
        }
        if (const auto* ins = entityAs<InsertData>(*e)) {
            const BlockDef* inner = doc.block(ins->block);
            if (!inner)
                continue;
            const Local il = localOf(doc, *inner, memo, depth + 1);
            if (!il.plain || (ins->array && !ins->array->swaps.empty())) {
                plain = false;
                break;
            }
            leaves = std::min<std::size_t>(
                leaves + il.leaves * instanceTransforms(*ins, inner->base).size(), 1u << 30);
        } else {
            ++leaves;
        }
    }
    loc.plain = plain;
    loc.leaves = leaves;
    if (plain)  // measured without a reference: its own attribute definitions are left out
        loc.box = contentBounds(doc, def, Transform2::identity(), depth, nullptr, memo, true);
    memo.blocks[def.id] = loc;
    return loc;
}

BBox2 contentBounds(const Document& doc, const BlockDef& def, const Transform2& t, int depth,
                    const InsertData* owner, Memo& memo, bool bypass) {
    BBox2 box;
    if (depth > kMaxNesting)
        return box;
    if (!bypass) {
        const Local loc = localOf(doc, def, memo, depth);
        // Moved into place: exact when the transform keeps the axes; for a turned view of a block
        // with very many objects the box of the turned box is used (a little larger, but cheap);
        // other turned views are measured once per angle and moved.
        if (loc.plain) {
            if (axisAligned(t) || loc.leaves > kLooseAbove) {
                box = t.applyToBox(loc.box);
            } else {
                Transform2 turn = t;
                turn.tx = 0;
                turn.ty = 0;
                const auto key = std::make_tuple(def.id.value, t.m00, t.m01, t.m10, t.m11);
                auto it = memo.turned.find(key);
                if (it == memo.turned.end())
                    it =
                        memo.turned
                            .emplace(key, contentBounds(doc, def, turn, depth, nullptr, memo, true))
                            .first;
                box = it->second;
                if (!box.empty()) {
                    box.min += Vec2{t.tx, t.ty};
                    box.max += Vec2{t.tx, t.ty};
                }
            }
            if (loc.attdefs && owner)
                for (EntityId id : def.entities) {
                    const Entity* e = doc.find(id);
                    const auto* td = e ? entityAs<TextData>(*e) : nullptr;
                    if (!td || !td->attdef || !e->common.visible)
                        continue;
                    if (auto shown = attributeText(*e, *owner)) {
                        auto moved = geom::transformed(*shown, t);
                        box.merge(moved ? geom::bounds(moved.value(), doc)
                                        : t.applyToBox(geom::bounds(*shown, doc)));
                    }
                }
            return box;
        }
    }
    std::optional<AsmResolved> res;
    const InsertData* eff = owner;
    if (def.assembly && owner) {
        res = resolveAssembly(doc, def, *owner);
        eff = &res->insert;
    }
    std::vector<Entity> storage;
    for (const Entity* e : blockItems(doc, def, eff, storage, res ? &res->nested : nullptr)) {
        if (!e->common.visible)
            continue;
        if (const auto* ins = entityAs<InsertData>(*e)) {
            if (const BlockDef* inner = doc.block(ins->block))
                for (const Transform2& it : instanceTransforms(*ins, inner->base))
                    box.merge(limitToClip(contentBounds(doc, *inner, t * it, depth + 1, ins, memo),
                                          *ins, t * it));
            for (const SwappedItem& sw : swappedItems(doc, *ins))
                if (const BlockDef* other = doc.block(sw.block))
                    box.merge(contentBounds(doc, *other, t * sw.t, depth + 1, nullptr, memo));
            continue;
        }
        std::optional<Entity> shown;
        if (const auto* td = entityAs<TextData>(*e); td && td->attdef) {
            if (!eff)
                continue;  // a bare definition (no insert) is measured by its tag below
            shown = attributeText(*e, *eff);
            if (!shown)
                continue;
            e = &*shown;
        }
        const auto start = profileBlocks() ? std::chrono::steady_clock::now()
                                           : std::chrono::steady_clock::time_point{};
        auto moved = geom::transformed(*e, t);
        if (moved) {
            box.merge(geom::bounds(moved.value(), doc));
        } else {
            box.merge(t.applyToBox(geom::bounds(*e, doc)));
        }
        if (profileBlocks()) {
            const double ms =
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
                    .count();
            if (ms > 20)
                std::fprintf(stderr,
                             "block profile: #%llu (kind %zu) in block '%s' measured in %.1f ms "
                             "(%s)\n",
                             static_cast<unsigned long long>(e->common.id.value), e->data.index(),
                             def.name.c_str(), ms,
                             moved ? "transformed" : "box of the untransformed object");
        }
    }
    return box;
}

}  // namespace

BoundsMemoScope::BoundsMemoScope() {
    if (!g_memo) {
        memo_ = std::make_shared<Memo>();
        g_memo = static_cast<Memo*>(memo_.get());
    }
}

BoundsMemoScope::~BoundsMemoScope() {
    if (memo_)
        g_memo = nullptr;
}

BBox2 insertBounds(const Document& doc, const Entity& insert) {
    const auto* ins = entityAs<InsertData>(insert);
    if (!ins)
        return {};
    const BlockDef* def = doc.block(ins->block);
    if (!def)
        return {};
    BBox2 box;
    Memo own;
    Memo& memo = g_memo ? *g_memo : own;
    for (const Transform2& t : instanceTransforms(*ins, def->base))
        box.merge(limitToClip(contentBounds(doc, *def, t, 0, ins, memo), *ins, t));
    for (const SwappedItem& sw : swappedItems(doc, *ins))
        if (const BlockDef* other = doc.block(sw.block))
            box.merge(contentBounds(doc, *other, sw.t, 1, nullptr, memo));
    return box;
}

namespace {

std::size_t countDrawn(const Document& doc, const BlockDef& def, int depth,
                       std::unordered_map<BlockId, std::size_t>& memo) {
    constexpr std::size_t kCap = std::size_t{1} << 40;
    if (depth > kMaxNesting)
        return 0;  // not drawn
    if (auto it = memo.find(def.id); it != memo.end())
        return it->second;
    memo[def.id] = 0;  // a block that contains itself
    std::size_t n = 0;
    for (EntityId id : def.entities) {
        const Entity* e = doc.find(id);
        const auto* ins = e ? entityAs<InsertData>(*e) : nullptr;
        const BlockDef* inner = ins ? doc.block(ins->block) : nullptr;
        if (!inner) {
            ++n;
            continue;
        }
        // The insert itself counts too: going through inserts is work even when they end in
        // nothing that is drawn.
        const std::size_t each = 1 + countDrawn(doc, *inner, depth + 1, memo);
        // (an insert without items still has its block built once)
        const std::size_t items =
            std::max<std::size_t>(instanceTransforms(*ins, inner->base).size(), 1);
        const std::size_t add = items > kCap / std::max<std::size_t>(each, 1) ? kCap : each * items;
        n = std::min(kCap, n + add);
    }
    memo[def.id] = n;
    return n;
}

}  // namespace

std::size_t expandedCount(const Document& doc, const Entity& insert) {
    const auto* ins = entityAs<InsertData>(insert);
    const BlockDef* def = ins ? doc.block(ins->block) : nullptr;
    if (!def)
        return 0;
    std::unordered_map<BlockId, std::size_t> memo;
    const std::size_t each = countDrawn(doc, *def, 0, memo);
    const std::size_t items = std::max<std::size_t>(instanceTransforms(*ins, def->base).size(), 1);
    return each > (std::size_t{1} << 40) / items ? std::size_t{1} << 40 : each * items;
}

BBox2 blockBounds(const Document& doc, BlockId block) {
    const BlockDef* def = doc.block(block);
    Memo own;
    Memo& memo = g_memo ? *g_memo : own;
    return def ? contentBounds(doc, *def, Transform2::identity(), 0, nullptr, memo) : BBox2{};
}

std::vector<Entity> snapGeometry(const Document& doc, const Entity& insert) {
    if (!entityAs<InsertData>(insert) || expandedCount(doc, insert) > kMaxExpanded)
        return {};  // (one drawn as its outline offers nothing inside it)
    detail::Flatten f{doc};
    f.expandNested = true;
    f.curvesOnly = true;
    const auto* ins = entityAs<InsertData>(insert);
    const BlockDef* def = doc.block(ins->block);
    if (!def)
        return {};
    for (const Transform2& t : instanceTransforms(*ins, def->base))
        f.contents(insert, t, 0);
    f.swapped(insert, Transform2::identity(), 0);
    return std::move(f.out);
}

}  // namespace tbcad::blocks
