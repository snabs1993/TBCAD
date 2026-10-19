// Drawing the same block many times: the shared list is prepared once and every reference only
// transforms and packs it (see Scene::upsertInstance).
#include <algorithm>
#include <cmath>
#include <iterator>

#include "scene_cursor.h"
#include "tbcad/render/fill.h"
#include "tbcad/render/scene.h"

namespace tbcad::render {

namespace {

constexpr double kMaxCoordinate = 1e15;
constexpr std::size_t kMaxTemplates = 2048;

std::uint32_t packRgba(Rgba c) {
    return static_cast<std::uint32_t>(c.r) | static_cast<std::uint32_t>(c.g) << 8 |
           static_cast<std::uint32_t>(c.b) << 16 | static_cast<std::uint32_t>(c.a) << 24;
}

bool drawable(const Vec2& p) {
    return isFinite(p) && std::abs(p.x) < kMaxCoordinate && std::abs(p.y) < kMaxCoordinate;
}

}  // namespace

// One batch worth of block-space geometry: segment end points (strokes), triangle corners
// (fills) or points.
struct Scene::InstanceTemplate {
    struct Group {
        BatchKind kind = BatchKind::Stroke;
        DisplayStyle style{};
        double halfWidthMm = 0;
        std::vector<Vec2> pts{};
    };
    std::shared_ptr<const DisplayList> keep{};
    bool usable = false;
    std::vector<Group> groups{};
    BBox2 box{};  // of everything, in block space
};

void Scene::InstanceTemplateDeleter::operator()(InstanceTemplate* t) const {
    delete t;
}

const Scene::InstanceTemplate* Scene::templateFor(const std::shared_ptr<const DisplayList>& list) {
    auto it = templates_.find(list.get());
    if (it != templates_.end())
        return it->second.get();
    // A block's geometry is built again after every edit (the block cache is per revision), so
    // templates of lists nobody else holds any more are dropped before they pile up: each keeps
    // its list, packed copies of every vertex and all.
    if (templates_.size() >= templateSweepAt_) {
        for (auto t = templates_.begin(); t != templates_.end();)
            t = t->second->keep.use_count() == 1 ? templates_.erase(t) : std::next(t);
        if (templates_.size() >= kMaxTemplates)
            templates_.clear();
        templateSweepAt_ = std::max<std::size_t>(64, templates_.size() * 2);
    }
    std::unique_ptr<InstanceTemplate, InstanceTemplateDeleter> t(new InstanceTemplate);
    t->keep = list;
    t->usable = true;
    auto groupFor = [&](BatchKind kind, const DisplayStyle& style) -> InstanceTemplate::Group& {
        for (auto& g : t->groups)
            if (g.kind == kind && g.style.layer == style.layer &&
                packRgba(g.style.color) == packRgba(style.color) &&
                (kind != BatchKind::Stroke ||
                 std::lround(std::max(0.0, g.style.lineweight) * 1000) ==
                     std::lround(std::max(0.0, style.lineweight) * 1000))) {
                return g;
            }
        t->groups.push_back({kind, style, 0, {}});
        t->groups.back().halfWidthMm =
            kind == BatchKind::Stroke ? std::max(0.0, style.lineweight) * 0.5 : 0;
        return t->groups.back();
    };
    for (const DisplayItem& item : list->items) {
        if (const auto* pl = std::get_if<DisplayPolyline>(&item)) {
            if (pl->style.pattern && !pl->style.pattern->empty()) {
                t->usable = false;
                break;
            }
            const std::size_t n = pl->pts.size();
            if (n < 2)
                continue;
            auto& g = groupFor(BatchKind::Stroke, pl->style);
            const std::size_t segments = (pl->closed && n > 2) ? n : n - 1;
            for (std::size_t i = 0; i < segments; ++i) {
                const Vec2 a = pl->pts[i], b = pl->pts[(i + 1) % n];
                if (!drawable(a) || !drawable(b) || distance(a, b) <= 0)
                    continue;
                g.pts.push_back(a);
                g.pts.push_back(b);
            }
        } else if (const auto* f = std::get_if<DisplayFill>(&item)) {
            if (isBackgroundColor(f->style.color)) {
                t->usable = false;
                break;
            }
            std::vector<Vec2> tris = triangulateEvenOdd(f->rings);
            auto& g = groupFor(BatchKind::Fill, f->style);
            for (std::size_t i = 0; i + 2 < tris.size(); i += 3) {
                g.pts.push_back(tris[i]);
                g.pts.push_back(tris[i + 1]);
                g.pts.push_back(tris[i + 2]);
            }
        } else if (const auto* p = std::get_if<DisplayPoints>(&item)) {
            auto& g = groupFor(BatchKind::Point, p->style);
            for (const Vec2& q : p->pts)
                if (drawable(q))
                    g.pts.push_back(q);
        }
    }
    for (const auto& g : t->groups)
        for (const Vec2& q : g.pts)
            t->box.expand(q);
    const InstanceTemplate* raw = t.get();
    templates_.emplace(list.get(), std::move(t));
    return raw;
}

bool Scene::upsertInstance(EntityId id, const std::shared_ptr<const DisplayList>& blockSpace,
                           const Transform2& t, std::uint32_t drawOrder, double builtPixelsPerMm) {
    return upsertInstance(id, blockSpace, std::span<const Transform2>(&t, 1), nullptr, drawOrder,
                          builtPixelsPerMm);
}

bool Scene::instanceUsable(const std::shared_ptr<const DisplayList>& blockSpace) {
    return blockSpace && templateFor(blockSpace)->usable;
}

void Scene::packInstance(const std::shared_ptr<const DisplayList>& blockSpace,
                         std::span<const Transform2> placements, InstancePack& out) const {
    out.used = 0;
    out.list = blockSpace.get();
    const auto found = templates_.find(blockSpace.get());
    if (found == templates_.end() || !found->second->usable)
        return;
    const InstanceTemplate* tpl = found->second.get();
    auto newRun = [&](std::uint32_t group, const TileKey& tile) -> InstancePack::Run& {
        if (out.runs.size() == out.used)
            out.runs.emplace_back();
        InstancePack::Run& r = out.runs[out.used++];
        r.group = group;
        r.tile = tile;
        r.box = BBox2{};
        r.verts.clear();
        return r;
    };
    auto origin = [&](const TileKey& tk) {
        return Vec2{static_cast<double>(tk.x) * tileSize_, static_cast<double>(tk.y) * tileSize_};
    };
    for (const Transform2& t : placements) {
        // The usual case: the whole reference lies in one tile. Then the geometry is only moved
        // and packed.
        if (!tpl->box.empty()) {
            const BBox2 where = t.applyToBox(tpl->box);
            if (drawable(where.min) && drawable(where.max)) {
                const TileKey tk = tileOf(where.min);
                if (tk == tileOf(where.max)) {
                    const Vec2 o = origin(tk);
                    for (std::size_t gi = 0; gi < tpl->groups.size(); ++gi) {
                        const InstanceTemplate::Group& g = tpl->groups[gi];
                        if (g.pts.empty())
                            continue;
                        InstancePack::Run& r = newRun(static_cast<std::uint32_t>(gi), tk);
                        r.verts.resize(g.pts.size());
                        for (std::size_t i = 0; i < g.pts.size(); ++i)
                            r.verts[i] = packVertex(t.apply(g.pts[i]), o);
                        r.box = where;
                    }
                    continue;
                }
            }
        }
        // The reference spans several tiles: its pieces go into one run per tile, so each tile
        // gets one range of this object however often consecutive pieces change tiles.
        for (std::size_t gi = 0; gi < tpl->groups.size(); ++gi) {
            const InstanceTemplate::Group& g = tpl->groups[gi];
            if (g.pts.empty())
                continue;
            const std::size_t step = g.kind == BatchKind::Stroke ? 2
                                     : g.kind == BatchKind::Fill ? 3
                                                                 : 1;
            const std::size_t first = out.used;
            std::size_t last = first;
            for (std::size_t i = 0; i + step <= g.pts.size(); i += step) {
                Vec2 q[3];
                for (std::size_t k = 0; k < step; ++k)
                    q[k] = t.apply(g.pts[i + k]);
                bool ok = true;
                for (std::size_t k = 0; k < step; ++k)
                    ok = ok && drawable(q[k]);
                if (!ok)
                    continue;
                // The tile is that of the first end point (strokes, points) or the centroid
                // (fills), as when the transformed list is added.
                const Vec2 at = step == 3 ? (q[0] + q[1] + q[2]) / 3.0 : q[0];
                const TileKey here = tileOf(at);
                if (out.used == first || !(out.runs[last].tile == here)) {
                    last = out.used;
                    for (std::size_t r = first; r < out.used; ++r)
                        if (out.runs[r].tile == here) {
                            last = r;
                            break;
                        }
                    if (last == out.used)
                        newRun(static_cast<std::uint32_t>(gi), here);
                }
                InstancePack::Run& run = out.runs[last];
                const Vec2 o = origin(run.tile);
                for (std::size_t k = 0; k < step; ++k) {
                    run.verts.push_back(packVertex(q[k], o));
                    run.box.expand(q[k]);
                }
            }
        }
    }
}

bool Scene::commitInstance(EntityId id, const InstancePack& pack, const DisplayList* extra,
                           std::uint32_t drawOrder, double builtPixelsPerMm) {
    const auto found = templates_.find(pack.list);
    if (!pack.list || found == templates_.end() || !found->second->usable)
        return false;
    const InstanceTemplate* tpl = found->second.get();
    if (extra)
        for (const DisplayItem& item : extra->items)
            if (const auto* f = std::get_if<DisplayFill>(&item);
                f && isBackgroundColor(f->style.color))
                return false;
    remove(id);
    EntityRec& rec = entities_[id];
    rec.drawOrder = drawOrder;
    Cursor c;
    c.entity = id;
    c.rec = &rec;
    c.drawOrder = drawOrder;
    c.level = levelBelow(drawOrder);
    c.maskLevel = c.level;
    c.builtZoom = static_cast<float>(builtPixelsPerMm);
    for (std::size_t r = 0; r < pack.used; ++r) {
        const InstancePack::Run& run = pack.runs[r];
        if (run.verts.empty())
            continue;
        const InstanceTemplate::Group& g = tpl->groups[run.group];
        const BatchKey key = styleKey(g.kind, g.style, c.level);
        c.batch = nullptr;
        c.tile = nullptr;
        c.append(*this, run.tile, key, g.halfWidthMm, run.verts);
        c.expand(run.box.min);
        c.expand(run.box.max);
    }
    // What differs from reference to reference (attribute values) comes as an ordinary list.
    if (extra) {
        for (const DisplayItem& item : extra->items) {
            c.batch = nullptr;
            c.tile = nullptr;
            if (const auto* pl = std::get_if<DisplayPolyline>(&item))
                addPolyline(c, *pl);
            else if (const auto* f = std::get_if<DisplayFill>(&item))
                addFill(c, *f);
            else if (const auto* p = std::get_if<DisplayPoints>(&item))
                addPoints(c, *p);
        }
    }
    finishRefs(rec);
    ++changeCounter_;
    return true;
}

bool Scene::upsertInstance(EntityId id, const std::shared_ptr<const DisplayList>& blockSpace,
                           std::span<const Transform2> placements, const DisplayList* extra,
                           std::uint32_t drawOrder, double builtPixelsPerMm) {
    if (!instanceUsable(blockSpace))
        return false;
    thread_local InstancePack pack;
    packInstance(blockSpace, placements, pack);
    return commitInstance(id, pack, extra, drawOrder, builtPixelsPerMm);
}

}  // namespace tbcad::render
