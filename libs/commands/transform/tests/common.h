#pragma once

#include <cmath>
#include <vector>

#include "tbcad/cmd/cmd.h"
#include "tbcad/cmd/register_all.h"

namespace tbtest {

using namespace tbcad;

inline bool near(Vec2 a, Vec2 b, double eps = 1e-9) {
    return distance(a, b) < eps;
}

// Block that add() fills (null = model space); tests set it to build a sheet-like owner.
inline BlockId& addOwner() {
    static BlockId owner{};
    return owner;
}

inline EntityId add(Document& doc, EntityData data) {
    auto tx = doc.begin("setup");
    tx.setDefaultOwner(addOwner());
    auto id = tx.add(Entity{{}, std::move(data)});
    tx.commit();
    return id.value();
}

template <class T>
const T& get(const Document& doc, EntityId id) {
    return *entityAs<T>(*doc.find(id));
}

struct Fixture {
    Document doc;
    cmd::Registry reg;
    cmd::ScriptHost host;
    Fixture() : host(doc, (registerAllCommands(reg), reg)) {}
};

}  // namespace tbtest
