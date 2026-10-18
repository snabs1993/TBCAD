#include <doctest/doctest.h>

#include <cctype>
#include <cmath>
#include <string>
#include <vector>

#include "tbcad/cmd/cmd.h"
#include "tbcad/cmd/register_all.h"
#include "tbcad/core/dump.h"
#include "tbcad/modify/modify.h"

using namespace tbcad;
using cmd::ScriptItem;

namespace {

struct Rig {
    Document doc;
    cmd::Registry reg;
    cmd::ScriptHost host;
    Rig() : host(doc, reg) { registerAllCommands(reg); }
    EntityId add(EntityData d) {
        Transaction tx = doc.begin("setup");
        auto id = tx.add(Entity{{}, std::move(d)});
        REQUIRE(id.ok());
        tx.commit();
        return id.value();
    }
};

struct Sample {
    const char* name;
    EntityData data;
    Vec2 p1, p2;  // two points on (or very near) the curve
};

std::vector<Sample> samples() {
    std::vector<Sample> s;
    s.push_back({"line", LineData{{0, 0}, {20, 0}}, {4, 0}, {14, 0}});
    s.push_back({"circle", CircleData{{10, 6}, 8}, {18, 6}, {10, 14}});
    ArcData arc{{10, 0}, 10, 0.2, 2.9};
    s.push_back({"arc",
                 arc,
                 {10 + 10 * std::cos(0.8), 10 * std::sin(0.8)},
                 {10 + 10 * std::cos(2.4), 10 * std::sin(2.4)}});
    PolylineData pl;
    pl.v = {{{0, 0}, 0.5, 0, 0}, {{10, 0}, 0, 0, 0}, {{10, 10}, -0.3, 0, 0}, {{0, 10}, 0, 0, 0}};
    s.push_back({"bulged polyline", pl, {10, 5}, {5, 10}});
    EllipseData el;
    el.center = {10, 4};
    el.majorAxis = {9, 3};
    el.ratio = 0.5;
    s.push_back({"ellipse", Indirect<EllipseData>(el), {19, 7}, {1, 1}});
    SplineData sp;
    sp.degree = 3;
    sp.knots = {0, 0, 0, 0, 1, 1, 1, 1};
    sp.control = {{0, 0}, {5, 10}, {10, -10}, {20, 0}};
    s.push_back({"spline", Indirect<SplineData>(sp), {0, 0}, {20, 0}});
    return s;
}

bool finiteDump(const std::string& d) {
    std::string low = d;
    for (auto& c : low)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return low.find("nan") == std::string::npos && low.find("inf") == std::string::npos;
}

}  // namespace

TEST_CASE("BREAK and LENGTHEN on every curve kind keep numbers finite and undo exactly") {
    const auto ss = samples();
    for (std::size_t i = 0; i < ss.size(); ++i) {
        CAPTURE(ss[i].name);
        Rig r;
        std::vector<EntityId> ids;
        for (const auto& s : ss)
            ids.push_back(r.add(s.data));
        const std::string before = canonicalDump(r.doc);
        r.host.run("BREAK", {ScriptItem::pick(ids[i], ss[i].p1),
                             std::to_string(ss[i].p2.x) + "," + std::to_string(ss[i].p2.y)});
        const std::string broken = canonicalDump(r.doc);
        CHECK(finiteDump(broken));
        if (broken != before) {
            CHECK(r.host.undo());
            CHECK(canonicalDump(r.doc) == before);
        }
        r.host.run("LENGTHEN",
                   {"Delta", "3", ScriptItem::pick(ids[i], ss[i].p2), ScriptItem::enter()});
        const std::string longer = canonicalDump(r.doc);
        CHECK(finiteDump(longer));
        if (longer != before) {
            CHECK(r.host.undo());
            CHECK(canonicalDump(r.doc) == before);
        }
    }
}

TEST_CASE("FILLET and CHAMFER between every pair of curve kinds are finite and undo exactly") {
    const auto ss = samples();
    int changed = 0;
    for (std::size_t i = 0; i < ss.size(); ++i) {
        for (std::size_t j = 0; j < ss.size(); ++j) {
            if (i == j)
                continue;
            CAPTURE(ss[i].name);
            CAPTURE(ss[j].name);
            for (int mode = 0; mode < 2; ++mode) {
                Rig r;
                std::vector<EntityId> ids;
                for (const auto& s : ss)
                    ids.push_back(r.add(s.data));
                const std::string before = canonicalDump(r.doc);
                if (mode == 0)
                    r.host.run("FILLET", {"R", "2", ScriptItem::pick(ids[i], ss[i].p1),
                                          ScriptItem::pick(ids[j], ss[j].p2)});
                else
                    r.host.run("CHAMFER", {"D", "1", "1", ScriptItem::pick(ids[i], ss[i].p1),
                                           ScriptItem::pick(ids[j], ss[j].p2)});
                const std::string after = canonicalDump(r.doc);
                CHECK(finiteDump(after));
                if (after != before) {
                    ++changed;
                    CHECK(r.host.undo());
                    CHECK(canonicalDump(r.doc) == before);
                }
            }
        }
    }
    MESSAGE("FILLET/CHAMFER changed the drawing in " << changed << " of 60 runs");
    CHECK(changed > 0);
}
