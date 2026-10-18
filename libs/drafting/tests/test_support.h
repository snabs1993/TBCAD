#pragma once

#include <cmath>

#include "tbcad/cmd/cmd.h"
#include "tbcad/drafting/drafting.h"
#include "tbcad/geom/geom.h"
#include "tbcad/index/document_indexer.h"
#include "tbcad/index/spatial_index.h"

namespace tbcad::drafting::test {

inline bool near(Vec2 a, Vec2 b, double eps = 1e-6) {
    return std::abs(a.x - b.x) <= eps && std::abs(a.y - b.y) <= eps;
}

// TWOLINE: two points make a line (the base of the second point is the first).
class TwoLine final : public cmd::Command {
public:
    std::string_view name() const override { return "TWOLINE"; }
    cmd::Task run(cmd::CommandContext& ctx) override {
        auto a = co_await ctx.getPoint("First point");
        if (!a.ok())
            co_return;
        cmd::PointOptions o;
        o.base = a.point;
        auto b = co_await ctx.getPoint("Second point", o);
        if (!b.ok())
            co_return;
        (void)ctx.tx().add(Entity{{}, LineData{a.point, b.point}});
    }
};

// A document with its spatial index, a script host and the drafting filters installed in the
// order the application uses.
struct World {
    Document doc;
    index::SpatialIndex idx;
    index::DocumentIndexer indexer{doc, idx,
                                   [this](const Entity& e) { return geom::bounds(e, doc); }};
    cmd::Registry reg;
    cmd::ScriptHost host;
    DraftingEnv env;
    OsnapFilter osnap{&env};
    OtrackFilter otrack{&env, &osnap};
    OtrackAcquirer otrackAcquire{&env, &osnap, &otrack};
    DynamicUcsFilter dynUcs{&env};
    PolarOrthoFilter polar{&env};
    GridSnapFilter grid{&env};
    DynamicInput dyn{&env};
    TypedPointParser parser{&env};

    World() : host(doc, reg) {
        (void)reg.add(std::make_unique<TwoLine>());
        env.doc = &doc;
        env.index = &idx;
        env.host = &host;
        host.filters().add(&dyn, kDynamicInputOrder);
        host.filters().add(&otrackAcquire, kOtrackAcquireOrder);
        host.filters().add(&osnap, kOsnapOrder);
        host.filters().add(&otrack, kOtrackOrder);
        host.filters().add(&dynUcs, kDynamicUcsOrder);
        host.filters().add(&polar, kPolarOrder);
        host.filters().add(&grid, kGridSnapOrder);
        host.runner().setLineParser(&parser);
        set(keys::osnap, "0");  // tests switch on what they need
    }
    void set(const char* key, const std::string& v) { host.setSetting(key, v); }

    EntityId add(Entity e) {
        auto tx = doc.begin("add");
        auto id = tx.add(std::move(e));
        (void)tx.commit();
        return id.value();
    }
    EntityId line(Vec2 a, Vec2 b) { return add(Entity{{}, LineData{a, b}}); }
    EntityId circle(Vec2 c, double r) { return add(Entity{{}, CircleData{c, r}}); }

    // Runs the chain like a hover at `cursor` (pixel size 1 mm unless given).
    Vec2 hover(Vec2 cursor, std::optional<Vec2> base = std::nullopt, double pixelSize = 1,
               cmd::FilterFeedback* fb = nullptr, bool shift = false, bool click = false) {
        cmd::PointQuery q;
        q.base = base;
        q.pixelSize = pixelSize;
        q.shift = shift;
        q.click = click;
        cmd::FilterFeedback local;
        cmd::FilterFeedback& out = fb ? *fb : local;
        const Vec2 p = host.filters().apply(q, cursor, out);
        out.hasPoint = true;
        out.base = base;
        out.point = p;
        return p;
    }
};

}  // namespace tbcad::drafting::test
