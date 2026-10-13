#pragma once

#include <doctest/doctest.h>

#include <cmath>

#include "tbcad/blocks/tbcad_blocks.h"
#include "tbcad/cmd/cmd.h"
#include "tbcad/cmd/register_all.h"

namespace tbcad::block_cmds::test {

struct Fixture {
    Document doc;
    cmd::Registry reg;
    std::unique_ptr<cmd::ScriptHost> host;

    Fixture() {
        registerAllCommands(reg);
        host = std::make_unique<cmd::ScriptHost>(doc, reg);
    }

    EntityId add(Entity e) {
        Transaction tx = doc.begin("add");
        EntityId id = tx.add(std::move(e)).value();
        tx.commit();
        return id;
    }
    EntityId addLine(double x0, double y0, double x1, double y1) {
        Entity e;
        e.data = LineData{{x0, y0}, {x1, y1}};
        return add(std::move(e));
    }
    EntityId addCircle(double x, double y, double r) {
        Entity e;
        e.data = CircleData{{x, y}, r};
        return add(std::move(e));
    }
    std::vector<EntityId> modelSpace() const { return doc.modelSpace(); }
};

inline bool near(Vec2 a, Vec2 b, double eps = 1e-9) {
    return distance(a, b) <= eps;
}

}  // namespace tbcad::block_cmds::test
