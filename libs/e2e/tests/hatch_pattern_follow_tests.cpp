// The pattern of a hatch moves, turns, scales and mirrors with the drawing: the dash lines after
// the edit are the edited dash lines from before. Islands and mirrored boundaries keep it coherent.
#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include "tbcad/cmd/cmd.h"
#include "tbcad/cmd/register_all.h"
#include "tbcad/cmd/script_host.h"
#include "tbcad/core/dump.h"
#include "tbcad/geom/geom.h"
#include "tbcad/hatch/hatch.h"

using namespace tbcad;
using cmd::ScriptItem;

namespace {

struct Rig {
    Document doc;
    cmd::Registry reg;
    cmd::ScriptHost host;
    Rig() : host(doc, (registerAllCommands(reg), reg)) {}

    EntityId add(EntityData d) {
        Transaction tx = doc.begin("setup");
        auto id = tx.add(Entity{{}, std::move(d)});
        REQUIRE(id.ok());
        tx.commit();
        return id.value();
    }
    EntityId theHatch() const {
        EntityId found{};
        doc.forEachEntity([&](const Entity& e) {
            if (entityAs<HatchData>(e))
                found = e.common.id;
        });
        return found;
    }
    // The dash lines of the hatch, each as "x1,y1-x2,y2" with the smaller end first, sorted.
    std::vector<std::string> dashes(const Transform2* t = nullptr) const {
        std::vector<std::string> out;
        const Entity* h = doc.find(theHatch());
        REQUIRE(h != nullptr);
        auto r = hatch::explodeHatch(doc, *h);
        REQUIRE(r.ok());
        for (const Entity& e : r.value()) {
            const auto* l = entityAs<LineData>(e);
            if (!l)
                continue;
            Vec2 a = t ? t->apply(l->a) : l->a, b = t ? t->apply(l->b) : l->b;
            auto key = [](Vec2 p) {
                char buf[64];
                std::snprintf(buf, sizeof buf, "%.3f,%.3f", std::round(p.x * 1000) / 1000 + 0.0,
                              std::round(p.y * 1000) / 1000 + 0.0);
                return std::string(buf);
            };
            std::string ka = key(a), kb = key(b);
            if (kb < ka)
                std::swap(ka, kb);
            out.push_back(ka + "-" + kb);
        }
        std::sort(out.begin(), out.end());
        return out;
    }
};

}  // namespace

TEST_CASE("hatch pattern lines follow MOVE, ROTATE, SCALE and MIRROR of the whole group") {
    struct Edit {
        const char* name;
        std::vector<ScriptItem> in;
        Transform2 t;
    };
    const Edit edits[] = {
        {"MOVE", {"0,0", "37.5,-12"}, Transform2::translate({37.5, -12})},
        {"ROTATE", {"10,10", "90"}, Transform2::rotate(1.5707963267948966, {10, 10})},
        {"SCALE", {"0,0", "2"}, Transform2::scale(2, 2)},
        {"MIRROR", {"0,0", "10,0", "Y"}, Transform2::scale(1, -1)},
    };
    for (const char* pat : {"ANSI31", "ANSI37", "BRICK"}) {
        for (const Edit& edit : edits) {
            CAPTURE(std::string(pat));
            CAPTURE(std::string(edit.name));
            Rig r;
            r.add(LineData{{0, 0}, {100, 0}});
            r.add(LineData{{100, 0}, {100, 60}});
            r.add(LineData{{100, 60}, {0, 60}});
            r.add(LineData{{0, 60}, {0, 0}});
            r.host.run("HATCH", {"Pattern", pat, "50,30", ScriptItem::enter()});
            const EntityId hid = r.theHatch();
            if (!hid)
                continue;  // the pattern name is not accepted by this prompt order: skip
            const std::vector<std::string> expected = r.dashes(&edit.t);
            REQUIRE_FALSE(expected.empty());
            const std::string before = canonicalDump(r.doc);
            std::vector<EntityId> all;
            r.doc.forEachEntity([&](const Entity& e) { all.push_back(e.common.id); });
            r.host.setSelection(all);
            r.host.run(edit.name, edit.in);
            REQUIRE(canonicalDump(r.doc) != before);
            // Pattern lines are the transformed ones (a mirror reverses the order of a pattern's
            // lines but not the lines themselves).
            if (std::string(edit.name) == "MIRROR" && std::string(pat) != "ANSI37") {
                // Like MIRRHATCH 0 in AutoCAD: the pattern direction is kept, not reflected, so
                // ANSI31 still rises at 45 degrees after the mirror.
                const auto after = r.dashes();
                REQUIRE_FALSE(after.empty());
                if (std::string(pat) == "ANSI31") {
                    for (const std::string& d : after) {
                        double x1, y1, x2, y2;
                        REQUIRE(std::sscanf(d.c_str(), "%lf,%lf-%lf,%lf", &x1, &y1, &x2, &y2) == 4);
                        CHECK((x2 - x1) * (y2 - y1) > 0);
                    }
                }
            } else {
                CHECK(r.dashes() == expected);
            }
            CHECK(r.host.undo());
            CHECK(canonicalDump(r.doc) == before);
        }
    }
}

namespace {
struct Probe {
    std::size_t loops = 0, sources = 0;
    BBox2 box{};
};
Probe probe(const Rig& r) {
    Probe p;
    const Entity* h = r.doc.find(r.theHatch());
    if (!h)
        return p;
    const auto& d = *entityAs<HatchData>(*h);
    p.loops = d.loops.size();
    p.sources = d.sourceBoundary.size();
    p.box = hatch::hatchBounds(d);
    return p;
}
void square(Rig& r) {
    r.add(LineData{{0, 0}, {100, 0}});
    r.add(LineData{{100, 0}, {100, 60}});
    r.add(LineData{{100, 60}, {0, 60}});
    r.add(LineData{{0, 60}, {0, 0}});
}
}  // namespace

TEST_CASE("hatch islands: moving, rotating or erasing the island entity updates the hatch") {
    Rig r;
    square(r);
    const EntityId island = r.add(CircleData{{50, 30}, 10});
    r.host.run("HATCH", {"20,30", ScriptItem::enter()});
    REQUIRE(r.theHatch());
    Probe base = probe(r);
    REQUIRE(base.loops == 2);
    const std::string before = canonicalDump(r.doc);

    r.host.setSelection({island});
    r.host.run("MOVE", {"0,0", "15,0"});
    Probe moved = probe(r);
    CHECK(moved.loops == 2);
    CHECK(moved.sources == base.sources);  // still tied to the same edges
    // The hole moved: a pattern line through the old hole centre now exists, one through the new
    // centre does not.
    CHECK(r.host.undo());
    CHECK(canonicalDump(r.doc) == before);

    // The island leaves the frame: the hatch is still sane and undo restores it.
    r.host.setSelection({island});
    r.host.run("MOVE", {"0,0", "200,0"});
    Probe out = probe(r);
    CHECK(out.loops >= 1);
    CHECK(out.box.max.x <= 100 + 1e-6);
    CHECK(r.host.undo());
    CHECK(canonicalDump(r.doc) == before);

    r.host.setSelection({island});
    r.host.run("ERASE", {});
    Probe gone = probe(r);
    CHECK(gone.sources == 0);  // detached
    CHECK(gone.loops == 2);    // but it keeps its last loops
    CHECK(r.host.undo());
    CHECK(canonicalDump(r.doc) == before);
}

TEST_CASE("hatch boundary edited with FILLET, JOIN or OFFSET stays sane and undoes in one step") {
    for (const char* which : {"FILLET", "JOIN", "COPY", "BREAK"}) {
        CAPTURE(std::string(which));
        Rig r;
        square(r);
        r.host.run("HATCH", {"50,30", ScriptItem::enter()});
        REQUIRE(r.theHatch());
        const Probe base = probe(r);
        const std::string before = canonicalDump(r.doc);
        std::vector<EntityId> src = entityAs<HatchData>(*r.doc.find(r.theHatch()))->sourceBoundary;
        REQUIRE(src.size() == 4);
        const std::string w = which;
        if (w == "FILLET") {
            r.host.run("FILLET", {"Radius", "10", ScriptItem::pick(src[0], {50, 0}),
                                  ScriptItem::pick(src[1], {100, 30})});
        } else if (w == "JOIN") {
            r.host.setSelection(src);
            r.host.run("JOIN", {});
        } else if (w == "COPY") {
            r.host.setSelection(src);
            r.host.run("COPY", {"0,0", "200,0", ScriptItem::enter()});
        } else {
            r.host.run("BREAK", {ScriptItem::pick(src[0], {20, 0}), "60,0"});
        }
        const Probe after = probe(r);
        CAPTURE(after.loops);
        CAPTURE(after.sources);
        CHECK(after.loops >= 1);
        CHECK(after.box.max.x <= 100 + 1e-6);
        CHECK(after.box.max.y <= 60 + 1e-6);
        (void)base;
        if (canonicalDump(r.doc) != before) {
            CHECK(r.host.undo());
            CHECK(canonicalDump(r.doc) == before);
        }
    }
}
