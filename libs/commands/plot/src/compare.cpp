// COMPARE: marks what differs between the open drawing and an earlier revision of it. Objects only
// in the open drawing are boxed green (added), objects only in the file red (removed) and objects
// that moved or changed yellow. The boxes go on their own layers; the list goes to the command
// line and the clipboard text.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>

#include "tbcad/core/dump.h"
#include "tbcad/geom/entity.h"
#include "tbcad/platform/paths.h"
#include "tbcad/plot/plot.h"

namespace tbcad::plot {

namespace {

constexpr const char* kAdded = "COMPARE-ADDED";
constexpr const char* kRemoved = "COMPARE-REMOVED";
constexpr const char* kChanged = "COMPARE-CHANGED";

const char* kindLabel(EntityKind k) {
    static const char* names[] = {"Line",   "Circle", "Arc",   "Polyline", "Point",  "Ellipse",
                                  "Spline", "Text",   "MText", "Hatch",    "Insert", "Dimension",
                                  "Leader", "Proxy",  "Table", "Ray",      "MLine"};
    const auto i = static_cast<std::size_t>(k);
    return i < sizeof names / sizeof *names ? names[i] : "?";
}

struct Item {
    EntityKind kind{};
    std::string layer;
    std::string sig;
    BBox2 box;
    bool matched = false;
};

std::string rounded(double v) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.3f", std::abs(v) < 5e-4 ? 0.0 : v);
    return buf;
}

// Two objects are the same when kind, layer, colour and shape agree. The shape of the simple
// curves is their full data; for the others (which carry style or block ids that differ between
// two files) it is the kind and the extents.
Item describe(const Document& doc, const Entity& e) {
    Item it;
    it.kind = kind(e);
    const Layer* ly = doc.layer(e.common.layer);
    it.layer = ly ? ly->name : std::string("0");
    it.box = geom::bounds(e, doc);
    std::string geomSig;
    switch (it.kind) {
        case EntityKind::Line:
        case EntityKind::Circle:
        case EntityKind::Arc:
        case EntityKind::Polyline:
        case EntityKind::Point:
        case EntityKind::Ellipse:
        case EntityKind::Spline: {
            Entity bare = e;
            bare.common = EntityCommon{};
            geomSig = dumpEntity(bare);
            break;
        }
        default:
            break;
    }
    if (geomSig.empty() && !it.box.empty())
        geomSig = rounded(it.box.min.x) + "," + rounded(it.box.min.y) + "," +
                  rounded(it.box.max.x) + "," + rounded(it.box.max.y);
    const Color& c = e.common.color;
    it.sig = std::to_string(static_cast<int>(it.kind)) + "|" + it.layer + "|" +
             std::to_string(static_cast<int>(c.kind)) + ":" + std::to_string(c.r) + "," +
             std::to_string(c.g) + "," + std::to_string(c.b) + "|" + geomSig;
    return it;
}

std::vector<Item> modelItems(const Document& doc) {
    std::vector<Item> out;
    for (EntityId id : doc.modelSpace())
        if (const Entity* e = doc.find(id))
            out.push_back(describe(doc, *e));
    return out;
}

Vec2 centerOf(const BBox2& b) {
    return Vec2{(b.min.x + b.max.x) / 2, (b.min.y + b.max.y) / 2};
}

LayerId layerFor(Transaction& tx, const Document& doc, const char* name, int aci) {
    if (const Layer* l = doc.layerByName(name))
        return l->id;
    Layer row;
    row.name = name;
    row.color = Color::index(static_cast<std::uint8_t>(aci));
    auto r = tx.addLayer(row);
    return r ? r.value() : kLayerZero;
}

std::string describeBox(const Item& it) {
    std::string s = std::string(kindLabel(it.kind)) + " on layer " + it.layer;
    if (!it.box.empty())
        s += " near " + rounded(centerOf(it.box).x) + ", " + rounded(centerOf(it.box).y);
    return s;
}

class CompareCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "COMPARE"; }
    std::vector<std::string> defaultAliases() const override { return {"DRAWINGCOMPARE"}; }
    std::string_view description() const override {
        return "Mark what was added, removed or changed since an earlier revision of the drawing";
    }

    cmd::Task run(cmd::CommandContext& ctx) override {
        auto f = co_await ctx.getString("Earlier revision to compare with (.tbcad or .dxf)");
        if (!f.ok() || f.text.empty())
            co_return;
        auto other = readDrawingFile(platform::pathFromUtf8(f.text));
        if (!other) {
            ctx.message(other.error().message);
            co_return;
        }
        std::vector<Item> now = modelItems(ctx.doc());
        std::vector<Item> before = modelItems(*other.value());

        // Same kind, layer, colour and shape: unchanged.
        std::multimap<std::string, std::size_t> bySig;
        for (std::size_t i = 0; i < before.size(); ++i)
            bySig.emplace(before[i].sig, i);
        for (Item& n : now) {
            auto it = bySig.find(n.sig);
            if (it == bySig.end())
                continue;
            n.matched = true;
            before[it->second].matched = true;
            bySig.erase(it);
        }

        // What is left of the same kind and near each other is one object that changed.
        std::vector<std::pair<const Item*, const Item*>> changed;
        for (Item& n : now) {
            if (n.matched || n.box.empty())
                continue;
            Item* best = nullptr;
            double bestD = 0;
            const double reach = std::max(std::hypot(n.box.width(), n.box.height()), 1.0);
            for (Item& b : before) {
                if (b.matched || b.kind != n.kind || b.box.empty())
                    continue;
                const double d = std::hypot(centerOf(b.box).x - centerOf(n.box).x,
                                            centerOf(b.box).y - centerOf(n.box).y);
                if (d <= reach && (!best || d < bestD)) {
                    best = &b;
                    bestD = d;
                }
            }
            if (best) {
                best->matched = n.matched = true;
                changed.emplace_back(&n, best);
            }
        }

        std::vector<const Item*> added, removed;
        for (const Item& n : now)
            if (!n.matched)
                added.push_back(&n);
        for (const Item& b : before)
            if (!b.matched)
                removed.push_back(&b);

        if (added.empty() && removed.empty() && changed.empty()) {
            ctx.message("The drawings are the same.");
            co_return;
        }

        auto& tx = ctx.tx();
        const Document& doc = ctx.doc();
        std::vector<EntityId> made;
        auto mark = [&](const BBox2& box, LayerId layer) {
            if (box.empty())
                return;
            const BBox2 b =
                box.inflated(std::max(2.0, 0.05 * std::hypot(box.width(), box.height())));
            PolylineData pl;
            pl.closed = true;
            pl.v = {{{b.min.x, b.min.y}},
                    {{b.max.x, b.min.y}},
                    {{b.max.x, b.max.y}},
                    {{b.min.x, b.max.y}}};
            Entity e;
            e.common.layer = layer;
            e.data = std::move(pl);
            if (auto r = tx.add(std::move(e)))
                made.push_back(r.value());
        };
        const LayerId lAdd = layerFor(tx, doc, kAdded, 3);
        const LayerId lRem = layerFor(tx, doc, kRemoved, 1);
        const LayerId lChg = layerFor(tx, doc, kChanged, 2);
        std::string full;
        for (const Item* n : added) {
            mark(n->box, lAdd);
            full += "Added: " + describeBox(*n) + "\n";
        }
        for (const Item* b : removed) {
            mark(b->box, lRem);
            full += "Removed: " + describeBox(*b) + "\n";
        }
        for (const auto& [n, b] : changed) {
            mark(n->box, lChg);
            full += "Changed: " + describeBox(*n) + "\n";
        }
        ctx.host().setSetting("clipboard.text", full);
        ctx.message(std::to_string(added.size()) + " added (green), " +
                    std::to_string(removed.size()) + " removed (red), " +
                    std::to_string(changed.size()) +
                    " changed (yellow). Boxes are on the COMPARE-* layers; the list is on the "
                    "clipboard.");
        ctx.host().setSelection(made);
    }
};

}  // namespace

std::unique_ptr<cmd::Command> makeCompareCommand() {
    return std::make_unique<CompareCommand>();
}

}  // namespace tbcad::plot
