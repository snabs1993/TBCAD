#pragma once

#include <doctest/doctest.h>

#include <algorithm>
#include <vector>

#include "tbcad/cmd/cmd.h"
#include "tbcad/select/select.h"

namespace tbcad::select::test {

// A document plus a ScriptHost with the built-in and the selection commands registered.
struct Fixture {
    Document doc;
    cmd::Registry reg;
    cmd::ScriptHost host;

    Fixture() : host(doc, reg) {
        cmd::registerBuiltinCommands(reg);
        select::registerCommands(reg);
    }

    EntityId addEntity(EntityData d, LayerId layer = {}) {
        Transaction tx = doc.begin("setup");
        Entity e{{}, std::move(d)};
        e.common.layer = layer;
        auto id = tx.add(std::move(e));
        REQUIRE(id.ok());
        tx.commit();
        return id.value();
    }

    LayerId addLayer(const std::string& name, LayerId parent = {}) {
        Transaction tx = doc.begin("setup");
        Layer l;
        l.name = name;
        l.parent = parent;
        auto id = tx.addLayer(l);
        REQUIRE(id.ok());
        tx.commit();
        return id.value();
    }

    void modify(EntityId id, const std::function<void(Entity&)>& fn) {
        Transaction tx = doc.begin("setup");
        REQUIRE(tx.modify(id, fn).ok());
        tx.commit();
    }

    bool visible(EntityId id) const { return doc.find(id)->common.visible; }
};

inline bool has(const std::vector<EntityId>& v, EntityId id) {
    return std::find(v.begin(), v.end(), id) != v.end();
}

}  // namespace tbcad::select::test
