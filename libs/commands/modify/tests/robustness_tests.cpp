#include <doctest/doctest.h>

#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include "tbcad/cmd/cmd.h"
#include "tbcad/cmd/register_all.h"
#include "tbcad/core/dump.h"
#include "tbcad/io/tbcad.h"
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

TEST_CASE("block references: BREAK, LENGTHEN, FILLET, CHAMFER and STRETCH never corrupt them") {
    for (const char* which : {"BREAK", "LENGTHEN", "FILLET", "CHAMFER", "STRETCH"}) {
        CAPTURE(which);
        Rig r;
        BlockId post;
        {
            auto tx = r.doc.begin("block");
            post = tx.addBlock(BlockDef{.name = "Post"}).value();
            REQUIRE(tx.add(Entity{{}, LineData{{0, -5}, {0, 5}}}, post).ok());
            REQUIRE(tx.add(Entity{{}, CircleData{{0, 0}, 3}}, post).ok());
            tx.commit();
        }
        InsertData ins;
        ins.block = post;
        ins.position = {6, 0};
        EntityId b = r.add(ins);
        EntityId l = r.add(LineData{{0, 0}, {10, 0}});
        const std::string before = canonicalDump(r.doc);
        const std::string w = which;
        if (w == "BREAK")
            r.host.run("BREAK", {ScriptItem::pick(b, {6, 5}), "6,-5"});
        else if (w == "LENGTHEN")
            r.host.run("LENGTHEN",
                       {"Delta", "3", ScriptItem::pick(b, {6, 5}), ScriptItem::enter()});
        else if (w == "FILLET")
            r.host.run("FILLET",
                       {"R", "1", ScriptItem::pick(b, {6, 5}), ScriptItem::pick(l, {2, 0})});
        else if (w == "CHAMFER")
            r.host.run("CHAMFER",
                       {"D", "1", "1", ScriptItem::pick(b, {6, 5}), ScriptItem::pick(l, {2, 0})});
        else
            r.host.run("STRETCH", {"3,-8", "9,8", "0,0", "2,1"});
        const std::string after = canonicalDump(r.doc);
        CHECK(finiteDump(after));
        // The block definition itself must never be edited by a command on a reference.
        CHECK(r.doc.find(b) != nullptr);
        if (after != before) {
            CHECK(r.host.undo());
            CHECK(canonicalDump(r.doc) == before);
        }
    }
}

// Speed on a dense drawing: TRIM and OFFSET of one object must not scan or rebuild everything.
TEST_CASE("TRIM and OFFSET stay quick in a drawing of 50 000 objects") {
    Rig f;
    {
        Transaction tx = f.doc.begin("fill");
        for (int i = 0; i < 50000; ++i) {
            const double x = (i % 250) * 40.0, y = (i / 250) * 40.0;
            Entity e;
            if (i % 3 == 0)
                e.data = LineData{{x, y}, {x + 30, y + 10}};
            else if (i % 3 == 1)
                e.data = CircleData{{x + 15, y + 15}, 8};
            else
                e.data = LineData{{x + 5, y - 5}, {x + 12, y + 25}};
            REQUIRE(tx.add(std::move(e)).ok());
        }
        tx.commit();
    }
    // A long line crossing a few hundred objects.
    EntityId target = f.add(LineData{{-100, 4020}, {10100, 4020}});
    auto t0 = std::chrono::steady_clock::now();
    f.host.run("TRIM",
               {ScriptItem::enter(), ScriptItem::pick(target, {5000, 4020}), ScriptItem::esc()});
    const double trimMs =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    t0 = std::chrono::steady_clock::now();
    f.host.run("OFFSET",
               {"5", ScriptItem::pick(target, {200, 4020}), "200,4100", ScriptItem::esc()});
    const double offsetMs =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    MESSAGE("50k objects: TRIM " << trimMs << " ms, OFFSET " << offsetMs << " ms");
    CHECK(trimMs < 2000);  // debug build; about 30 ms measured
    CHECK(offsetMs < 2000);
    const auto* l = entityAs<LineData>(*f.doc.find(target));
    REQUIRE(l);
    CHECK(std::abs(l->b.x - l->a.x) < 10200);  // the pick was trimmed away or cut short
    CHECK(f.doc.modelSpace().size() >= 50001);
}

namespace {

std::vector<Sample> moreSamples() {
    std::vector<Sample> s = samples();
    SplineData fit;  // fit points only: no control points yet
    fit.degree = 3;
    fit.fit = {{0, 0}, {5, 6}, {10, -4}, {15, 5}, {20, 0}};
    s.push_back({"fit-only spline", Indirect<SplineData>(fit), {0, 0}, {20, 0}});
    SplineData closed;
    closed.degree = 3;
    closed.closed = true;
    closed.fit = {{0, 0}, {10, -4}, {14, 8}, {2, 10}};
    s.push_back({"closed fit spline", Indirect<SplineData>(closed), {0, 0}, {14, 8}});
    PolylineData cp;
    cp.closed = true;
    cp.v = {{{0, 0}, 0, 0, 0}, {{12, 0}, 0.4, 0, 0}, {{12, 9}, 0, 0, 0}, {{0, 9}, 0, 0, 0}};
    s.push_back({"closed polyline", cp, {12, 4}, {6, 9}});
    return s;
}

}  // namespace

TEST_CASE("modify commands on every curve kind, fit-only splines included, stay finite and undo") {
    const auto ss = moreSamples();
    struct Script {
        const char* command;
        // Items for the picked object `id` with two points on it; the crossing line is `edge`.
        std::vector<ScriptItem> (*items)(EntityId id, EntityId edge, Vec2 p1, Vec2 p2);
    };
    const std::vector<Script> scripts = {
        {"MIRROR",
         [](EntityId id, EntityId, Vec2, Vec2) -> std::vector<ScriptItem> {
             return {ScriptItem::select({id}), ScriptItem::enter(), "0,-5", "20,-5", "N"};
         }},
        {"ROTATE",
         [](EntityId id, EntityId, Vec2, Vec2) -> std::vector<ScriptItem> {
             return {ScriptItem::select({id}), ScriptItem::enter(), "5,5", "37"};
         }},
        {"SCALE",
         [](EntityId id, EntityId, Vec2, Vec2) -> std::vector<ScriptItem> {
             return {ScriptItem::select({id}), ScriptItem::enter(), "5,5", "1.7"};
         }},
        {"COPY",
         [](EntityId id, EntityId, Vec2, Vec2) -> std::vector<ScriptItem> {
             return {ScriptItem::select({id}), ScriptItem::enter(), "0,0", "3,4",
                     ScriptItem::enter()};
         }},
        {"OFFSET",
         [](EntityId id, EntityId, Vec2 p1, Vec2) -> std::vector<ScriptItem> {
             return {"1.5", ScriptItem::pick(id, p1), "10,30", ScriptItem::enter()};
         }},
        {"EXPLODE",
         [](EntityId id, EntityId, Vec2, Vec2) -> std::vector<ScriptItem> {
             return {ScriptItem::select({id}), ScriptItem::enter()};
         }},
        {"TRIM",
         [](EntityId id, EntityId edge, Vec2 p1, Vec2) -> std::vector<ScriptItem> {
             return {ScriptItem::select({edge}), ScriptItem::enter(), ScriptItem::pick(id, p1),
                     ScriptItem::enter()};
         }},
        {"EXTEND",
         [](EntityId id, EntityId edge, Vec2 p1, Vec2) -> std::vector<ScriptItem> {
             return {ScriptItem::select({edge}), ScriptItem::enter(), ScriptItem::pick(id, p1),
                     ScriptItem::enter()};
         }},
        {"BREAKATPOINT",
         [](EntityId id, EntityId, Vec2 p1, Vec2 p2) -> std::vector<ScriptItem> {
             return {ScriptItem::pick(id, p1), std::to_string(p2.x) + "," + std::to_string(p2.y)};
         }},
        {"DIVIDE",
         [](EntityId id, EntityId, Vec2 p1, Vec2) -> std::vector<ScriptItem> {
             return {ScriptItem::pick(id, p1), "6"};
         }},
        {"MEASURE",
         [](EntityId id, EntityId, Vec2 p1, Vec2) -> std::vector<ScriptItem> {
             return {ScriptItem::pick(id, p1), "4"};
         }},
        {"JOIN",
         [](EntityId id, EntityId edge, Vec2, Vec2) -> std::vector<ScriptItem> {
             return {ScriptItem::select({id, edge}), ScriptItem::enter()};
         }},
        {"PEDIT",
         [](EntityId id, EntityId, Vec2 p1, Vec2) -> std::vector<ScriptItem> {
             return {ScriptItem::pick(id, p1), "Yes", "Width", "2", ScriptItem::enter()};
         }},
        {"STRETCH",
         [](EntityId, EntityId, Vec2, Vec2) -> std::vector<ScriptItem> {
             return {"-1,-12", "21,12", "0,0", "3,2"};
         }},
        {"ALIGN",
         [](EntityId id, EntityId, Vec2, Vec2) -> std::vector<ScriptItem> {
             return {ScriptItem::select({id}),
                     ScriptItem::enter(),
                     "0,0",
                     "4,4",
                     "10,0",
                     "14,4",
                     ScriptItem::enter(),
                     "N"};
         }},
    };
    int changed = 0, runs = 0;
    for (const auto& sample : ss) {
        for (const Script& sc : scripts) {
            CAPTURE(sample.name);
            CAPTURE(sc.command);
            Rig r;
            const EntityId id = r.add(sample.data);
            // A vertical line through the middle of the object's box, to trim or extend against.
            const EntityId edge = r.add(LineData{{9, -20}, {9, 20}});
            const std::string before = canonicalDump(r.doc);
            r.host.run(sc.command, sc.items(id, edge, sample.p1, sample.p2));
            r.host.run("", {});
            const std::string after = canonicalDump(r.doc);
            ++runs;
            CHECK(finiteDump(after));
            if (after != before) {
                ++changed;
                while (r.host.undo()) {
                }
                CHECK(canonicalDump(r.doc) == before);
            }
        }
    }
    MESSAGE("changed the drawing in " << changed << " of " << runs << " runs");
    CHECK(changed > runs / 4);
}

TEST_CASE("EXTEND, JOIN and BREAKATPOINT work on splines, fit-only ones included") {
    SplineData ctrl;
    ctrl.degree = 3;
    ctrl.knots = {0, 0, 0, 0, 1, 1, 1, 1};
    ctrl.control = {{0, 0}, {5, 10}, {10, -10}, {20, 0}};
    SplineData fit;
    fit.degree = 3;
    fit.fit = {{0, 0}, {5, 6}, {10, -4}, {15, 5}, {20, 0}};
    struct Case {
        const char* name;
        SplineData data;
        Vec2 onCurve;  // a point in the middle of the curve
    };
    for (const Case& c :
         {Case{"control points", ctrl, {8.125, 0}}, Case{"fit points", fit, {10, -4}}}) {
        CAPTURE(c.name);
        {  // EXTEND to a boundary beyond the end at (20, 0)
            Rig r;
            const EntityId id = r.add(Indirect<SplineData>(c.data));
            const EntityId edge = r.add(LineData{{27, -20}, {27, 20}});
            const std::string before = canonicalDump(r.doc);
            r.host.run("EXTEND", {ScriptItem::select({edge}), ScriptItem::enter(),
                                  ScriptItem::pick(id, {19.5, 0}), ScriptItem::enter()});
            const Entity* e = r.doc.find(id);
            REQUIRE(e);
            const auto b = geom::bounds(*e, r.doc);
            CHECK(b.max.x == doctest::Approx(27).epsilon(0.01));
            CHECK(finiteDump(canonicalDump(r.doc)));
            CHECK(r.host.undo());
            CHECK(canonicalDump(r.doc) == before);
        }
        {  // JOIN with a line that starts where the spline ends
            Rig r;
            const EntityId id = r.add(Indirect<SplineData>(c.data));
            const EntityId ln = r.add(LineData{{20, 0}, {30, 5}});
            const std::string before = canonicalDump(r.doc);
            r.host.run("JOIN", {ScriptItem::select({id, ln}), ScriptItem::enter()});
            CHECK(r.doc.modelSpaceCount() == 1);
            CHECK(finiteDump(canonicalDump(r.doc)));
            CHECK(r.host.undo());
            CHECK(canonicalDump(r.doc) == before);
        }
        {  // BREAKATPOINT in the middle: two pieces that meet at the point
            Rig r;
            const EntityId id = r.add(Indirect<SplineData>(c.data));
            const std::string before = canonicalDump(r.doc);
            r.host.run("BREAKATPOINT",
                       {ScriptItem::pick(id, c.onCurve),
                        std::to_string(c.onCurve.x) + "," + std::to_string(c.onCurve.y)});
            CHECK(r.doc.modelSpaceCount() == 2);
            CHECK(finiteDump(canonicalDump(r.doc)));
            CHECK(r.host.undo());
            CHECK(canonicalDump(r.doc) == before);
        }
    }
}

// EXTEND follows the ray its end moves on through the spatial index; it must stop where the scan
// of every object stops, boundaries behind, parallel, touching the end or far away included.
TEST_CASE("EXTEND by the index gives the same result as testing every object") {
    std::mt19937 rng(7);
    auto rnd = [&](double lo, double hi) { return std::uniform_real_distribution<double>(lo, hi)(rng); };
    int extended = 0;
    for (int round = 0; round < 60; ++round) {
        struct Pair {
            Rig scan, indexed;
        };
        auto pair = std::make_unique<Pair>();
        std::vector<EntityData> edges;
        for (int i = 0; i < 40; ++i) {
            if (i % 4 == 0) {
                edges.push_back(CircleData{{rnd(-200, 200), rnd(-200, 200)}, rnd(2, 40)});
            } else {
                const Vec2 a{rnd(-200, 200), rnd(-200, 200)};
                edges.push_back(LineData{a, a + Vec2{rnd(-60, 60), rnd(-60, 60)}});
            }
        }
        // A target that is short, so the nearest boundary is often far and sometimes absent.
        const Vec2 a{rnd(-100, 100), rnd(-100, 100)};
        EntityData target;
        if (round % 3 == 2) {
            PolylineData pl;
            pl.v = {{a, 0, 0, 0}, {a + Vec2{rnd(2, 9), rnd(-4, 4)}, 0, 0, 0},
                    {a + Vec2{rnd(10, 20), rnd(-6, 6)}, 0, 0, 0}};
            target = pl;
        } else {
            target = LineData{a, a + Vec2{rnd(-12, 12), rnd(-12, 12)}};
        }
        EntityId idA{}, idB{};
        for (Rig* rig : {&pair->scan, &pair->indexed}) {
            for (const EntityData& d : edges)
                rig->add(d);
            (rig == &pair->scan ? idA : idB) = rig->add(target);
        }
        Rig& ix = pair->indexed;
        ix.host.setViewExtents(BBox2{{-400, -400}, {400, 400}});
        ix.host.setWindowQuery([&doc = ix.doc](const BBox2& w) {
            std::vector<EntityId> out;
            for (EntityId id : doc.modelSpace())
                if (const Entity* e = doc.find(id); e && geom::bounds(*e, doc).intersects(w))
                    out.push_back(id);
            return out;
        });
        const Vec2 pick = a + Vec2{rnd(-3, 3), rnd(-3, 3)};
        pair->scan.host.run("EXTEND", {ScriptItem::enter(), ScriptItem::pick(idA, pick),
                                       ScriptItem::esc()});
        ix.host.run("EXTEND", {ScriptItem::enter(), ScriptItem::pick(idB, pick), ScriptItem::esc()});
        const Entity* ea = pair->scan.doc.find(idA);
        const Entity* eb = ix.doc.find(idB);
        REQUIRE(ea);
        REQUIRE(eb);
        INFO("round " << round);
        if (const auto* la = entityAs<LineData>(*ea)) {
            const auto* lb = entityAs<LineData>(*eb);
            REQUIRE(lb);
            CHECK(distance(la->a, lb->a) < 1e-9);
            CHECK(distance(la->b, lb->b) < 1e-9);
            extended += distance(la->a, std::get<LineData>(target).a) +
                                    distance(la->b, std::get<LineData>(target).b) > 1e-9;
        } else if (const auto* pa = entityAs<PolylineData>(*ea)) {
            const auto* pb = entityAs<PolylineData>(*eb);
            REQUIRE(pb);
            REQUIRE(pa->v.size() == pb->v.size());
            for (std::size_t k = 0; k < pa->v.size(); ++k)
                CHECK(distance(pa->v[k].p, pb->v[k].p) < 1e-9);
        }
    }
    CHECK(extended > 10);  // the scenes do extend now and then
}

TEST_CASE("random sequences of modify commands stay finite and undo step by step") {
    std::mt19937 rng(std::getenv("TBCAD_FUZZ_SEED") ? std::strtoul(std::getenv("TBCAD_FUZZ_SEED"), nullptr, 10) : 20261019u);
    auto rnd = [&](double lo, double hi) { return std::uniform_real_distribution<double>(lo, hi)(rng); };
    int steps = 0, changedSteps = 0;
    for (int round = 0; round < 12; ++round) {
        Rig r;
        std::vector<EntityId> ids;
        const auto ss = samples();
        for (const auto& s : ss) {
            // Each sample at its own random place, so edges cross some of the others.
            const Vec2 shift{rnd(-8, 8), rnd(-8, 8)};
            EntityData d = s.data;
            ids.push_back(r.add(d));
            if (auto* l = std::get_if<LineData>(&d); l)
                (void)shift;
        }
        const auto pickOn = [&](std::size_t k) {
            return rng() % 2 ? ss[k].p1 : ss[k].p2;
        };
        // A command that waits for more input is cancelled so the next one can start.
        const auto go = [&](std::string_view name, std::vector<ScriptItem> items) {
            if (!r.host.run(name, items).finished)
                r.host.feed({ScriptItem::esc(), ScriptItem::esc(), ScriptItem::esc()});
        };
        std::vector<std::string> history{canonicalDump(r.doc)};
        for (int step = 0; step < 40; ++step) {
            const std::size_t a = rng() % ids.size(), b = rng() % ids.size();
            const std::string ra = std::to_string(rnd(0.2, 6.0)), rb = std::to_string(rnd(0.2, 6.0));
            switch (rng() % 17) {
                case 0:
                    go("TRIM", {ScriptItem::enter(), ScriptItem::pick(ids[a], pickOn(a % ss.size())),
                                        ScriptItem::enter()});
                    break;
                case 1:
                    go("EXTEND", {ScriptItem::enter(),
                                          ScriptItem::pick(ids[a], pickOn(a % ss.size())),
                                          ScriptItem::enter()});
                    break;
                case 2:
                    go("FILLET", {"R", ra, ScriptItem::pick(ids[a], pickOn(a % ss.size())),
                                          ScriptItem::pick(ids[b], pickOn(b % ss.size()))});
                    break;
                case 3:
                    go("CHAMFER", {"D", ra, rb, ScriptItem::pick(ids[a], pickOn(a % ss.size())),
                                           ScriptItem::pick(ids[b], pickOn(b % ss.size()))});
                    break;
                case 4:
                    go("OFFSET", {ra, ScriptItem::pick(ids[a], pickOn(a % ss.size())),
                                          std::to_string(rnd(-10, 25)) + "," + std::to_string(rnd(-10, 20)),
                                          ScriptItem::enter()});
                    break;
                case 5:
                    go("BREAK", {ScriptItem::pick(ids[a], pickOn(a % ss.size())),
                                         std::to_string(rnd(-5, 25)) + "," + std::to_string(rnd(-5, 15))});
                    break;
                case 6:
                    go("EXPLODE", {ScriptItem::select({ids[a]}), ScriptItem::enter()});
                    break;
                case 7:
                    go("JOIN", {ScriptItem::select({ids[a], ids[b]}), ScriptItem::enter()});
                    break;
                case 8:
                    go("MOVE", {ScriptItem::select({ids[a]}), ScriptItem::enter(), "0,0",
                                std::to_string(rnd(-9, 9)) + "," + std::to_string(rnd(-9, 9))});
                    break;
                case 9:
                    go("ROTATE", {ScriptItem::select({ids[a]}), ScriptItem::enter(), "5,5",
                                  std::to_string(rnd(-180, 180))});
                    break;
                case 10:
                    go("SCALE", {ScriptItem::select({ids[a]}), ScriptItem::enter(), "5,5",
                                 std::to_string(rnd(0.3, 2.5))});
                    break;
                case 11:
                    go("MIRROR", {ScriptItem::select({ids[a]}), ScriptItem::enter(), "0,0",
                                  std::to_string(rnd(-5, 5)) + "," + std::to_string(rnd(-5, 5)), "N"});
                    break;
                case 14: {
                    const double x = rnd(-2, 18), y = rnd(-4, 8);
                    go("STRETCH", {std::to_string(x) + "," + std::to_string(y),
                                   std::to_string(x + rnd(2, 12)) + "," + std::to_string(y + rnd(2, 12)),
                                   "0,0", std::to_string(rnd(-6, 6)) + "," + std::to_string(rnd(-6, 6))});
                    break;
                }
                case 15:
                    go("LENGTHEN", {"Delta", std::to_string(rnd(-4, 6)),
                                    ScriptItem::pick(ids[a], pickOn(a % ss.size())),
                                    ScriptItem::enter()});
                    break;
                case 16:
                    go("ARRAY", {ScriptItem::select({ids[a]}), ScriptItem::enter(), "R", "Rows", "2",
                                 "Columns", "2", "Spacing", std::to_string(rnd(3, 12)),
                                 std::to_string(rnd(3, 12)), ""});
                    break;
                case 12:
                    go("HATCH", {std::to_string(rnd(-2, 22)) + "," + std::to_string(rnd(-3, 12)),
                                 ScriptItem::enter()});
                    break;
                default:
                    go("COPY", {ScriptItem::select({ids[a]}), ScriptItem::enter(), "0,0",
                                std::to_string(rnd(-9, 9)) + "," + std::to_string(rnd(-9, 9)),
                                ScriptItem::enter()});
                    break;
            }
            ++steps;
            const std::string now = canonicalDump(r.doc);
            CHECK(finiteDump(now));
            const std::size_t depth = r.host.undoStack().undoCount();
            CHECK(depth + 1 >= history.size());
            if (depth == history.size()) {  // one new undo step
                if (now != history.back())
                    ++changedSteps;
                history.push_back(now);
            } else {
                // Nothing was recorded: the drawing must not have changed either.
                CHECK(depth == history.size() - 1);
                CHECK(now == history.back());
            }
        }
        // Undo one step at a time: each state must be the one before it; redo brings them back.
        for (std::size_t k = history.size() - 1; k > 0; --k) {
            CHECK(r.host.undo());
            CHECK(canonicalDump(r.doc) == history[k - 1]);
        }
        for (std::size_t k = 1; k < history.size(); ++k) {
            CHECK(r.host.redo());
            CHECK(canonicalDump(r.doc) == history[k]);
        }
    }
    MESSAGE(steps << " commands, " << changedSteps << " changed the drawing");
    CHECK(changedSteps > 30);
}

TEST_CASE("degenerate and huge geometry: modify commands never crash, stay finite and undo") {
    std::mt19937 rng(std::getenv("TBCAD_FUZZ_SEED") ? std::strtoul(std::getenv("TBCAD_FUZZ_SEED"), nullptr, 10) : 77u);
    auto rnd = [&](double lo, double hi) { return std::uniform_real_distribution<double>(lo, hi)(rng); };
    const auto pt = [](Vec2 p) { return std::to_string(p.x) + "," + std::to_string(p.y); };
    int changed = 0;
    for (int round = 0; round < 10; ++round) {
        Rig r;
        std::vector<EntityId> ids;
        std::vector<Vec2> spots;  // places worth picking
        const auto addE = [&](EntityData d, Vec2 spot) {
            ids.push_back(r.add(std::move(d)));
            spots.push_back(spot);
        };
        const double big = 1e9;
        addE(LineData{{5, 5}, {5, 5}}, {5, 5});                       // zero length
        addE(LineData{{0, 0}, {1e-9, 0}}, {0, 0});                    // tiny
        addE(LineData{{0, 0}, {20, 0}}, {10, 0});
        addE(LineData{{0, 0}, {20, 0}}, {10, 0});                     // duplicate
        addE(LineData{{-big, 3}, {big, 3}}, {0, 3});                  // huge
        addE(LineData{{big, big}, {big + 10, big + 5}}, {big + 5, big + 2});
        addE(CircleData{{10, 0}, 1e-6}, {10, 0});                     // tiny circle
        addE(CircleData{{0, 0}, 5}, {5, 0});
        addE(CircleData{{0, 0}, 5}, {-5, 0});                         // coincident
        addE(CircleData{{big, 0}, big}, {0, 0});                      // huge circle
        addE(ArcData{{3, 3}, 4, 1.0, 1.0 + 1e-9}, {3, 7});            // sliver arc
        addE(ArcData{{3, 3}, 4, 0.0, 6.2831853}, {7, 3});             // nearly full
        PolylineData z;  // repeated vertices, bulge on a zero segment
        z.v = {{{0, 0}, 0.5, 0, 0}, {{0, 0}, 0, 0, 0}, {{10, 0}, 1.0, 0, 0}, {{10, 0}, 0, 0, 0},
               {{10, 10}, 0, 0, 0}};
        addE(z, {5, 0});
        PolylineData one;
        one.v = {{{4, 4}, 0, 0, 0}};
        addE(one, {4, 4});
        PolylineData cl;
        cl.closed = true;
        cl.v = {{{0, 0}, 0, 0, 0}, {{8, 0}, 0, 0, 0}, {{8, 8}, 0, 0, 0}, {{0, 8}, 0, 0, 0}};
        addE(cl, {4, 0});
        const auto pickAt = [&](std::size_t k) {
            return spots[k] + Vec2{rng() % 2 ? 0.0 : rnd(-0.01, 0.01), rng() % 2 ? 0.0 : rnd(-0.01, 0.01)};
        };
        const auto go = [&](std::string_view name, std::vector<ScriptItem> items) {
            if (!r.host.run(name, items).finished)
                r.host.feed({ScriptItem::esc(), ScriptItem::esc(), ScriptItem::esc()});
        };
        std::vector<std::string> history{canonicalDump(r.doc)};
        for (int step = 0; step < 40; ++step) {
            const std::size_t a = rng() % ids.size(), b = rng() % ids.size();
            const std::string ra = std::to_string(rng() % 4 == 0 ? 0.0 : rnd(0.0, 8.0));
            const Vec2 where{rnd(-12, 25), rnd(-12, 20)};
            switch (rng() % 11) {
                case 0: go("TRIM", {ScriptItem::enter(), ScriptItem::pick(ids[a], pickAt(a)), ScriptItem::enter()}); break;
                case 1: go("EXTEND", {ScriptItem::enter(), ScriptItem::pick(ids[a], pickAt(a)), ScriptItem::enter()}); break;
                case 2: go("FILLET", {"R", ra, ScriptItem::pick(ids[a], pickAt(a)), ScriptItem::pick(ids[b], pickAt(b))}); break;
                case 3: go("CHAMFER", {"D", ra, ra, ScriptItem::pick(ids[a], pickAt(a)), ScriptItem::pick(ids[b], pickAt(b))}); break;
                case 4: go("OFFSET", {ra, ScriptItem::pick(ids[a], pickAt(a)), pt(where), ScriptItem::enter()}); break;
                case 5: go("BREAK", {ScriptItem::pick(ids[a], pickAt(a)), pt(where)}); break;
                case 6: go("EXPLODE", {ScriptItem::select({ids[a]}), ScriptItem::enter()}); break;
                case 7: go("JOIN", {ScriptItem::select({ids[a], ids[b]}), ScriptItem::enter()}); break;
                case 8: go("SCALE", {ScriptItem::select({ids[a]}), ScriptItem::enter(), "0,0", std::to_string(rng() % 3 == 0 ? 0.0 : rnd(-2, 3))}); break;
                case 9: go("LENGTHEN", {"Delta", std::to_string(rnd(-30, 30)), ScriptItem::pick(ids[a], pickAt(a)), ScriptItem::enter()}); break;
                default: go("HATCH", {pt(where), ScriptItem::enter()}); break;
            }
            const std::string now = canonicalDump(r.doc);
            CHECK(finiteDump(now));
            if (step % 8 == 7) {
                auto bytes = io::writeTbcadToMemory(r.doc);
                REQUIRE(bytes.ok());
                io::IoReport rep;
                auto back = io::readTbcad(std::span<const std::uint8_t>(bytes.value()), rep);
                REQUIRE(back.ok());
                CHECK(canonicalDump(*back.value()) == now);
            }
            const std::size_t depth = r.host.undoStack().undoCount();
            CHECK(depth + 1 >= history.size());
            if (depth == history.size()) {
                if (now != history.back())
                    ++changed;
                history.push_back(now);
            } else {
                CHECK(depth == history.size() - 1);
                CHECK(now == history.back());
            }
        }
        for (std::size_t k = history.size() - 1; k > 0; --k) {
            CHECK(r.host.undo());
            CHECK(canonicalDump(r.doc) == history[k - 1]);
        }
    }
    MESSAGE(changed << " steps changed the drawing");
    CHECK(changed > 20);
}

TEST_CASE("scenes with blocks, hatches and wide polylines: modify commands stay finite and undo") {
    std::mt19937 rng(std::getenv("TBCAD_FUZZ_SEED") ? std::strtoul(std::getenv("TBCAD_FUZZ_SEED"), nullptr, 10) : 31u);
    auto rnd = [&](double lo, double hi) { return std::uniform_real_distribution<double>(lo, hi)(rng); };
    const auto pt = [](Vec2 p) { return std::to_string(p.x) + "," + std::to_string(p.y); };
    int changed = 0;
    for (int round = 0; round < 8; ++round) {
        Rig r;
        // A small block, then inserts at odd scales and angles (mirrored and uneven too).
        r.add(LineData{{0, 0}, {6, 0}});
        EntityId seed{};
        r.doc.forEachEntity([&](const Entity& e) { seed = e.common.id; });
        r.host.setSelection({seed});
        r.host.run("BLOCK", {"Mark", "0,0", "D"});
        for (int k = 0; k < 3; ++k) {
            Rig* rr = &r;
            rr->host.run("INSERT", {"Mark", pt({rnd(0, 20), rnd(0, 12)})});
        }
        PolylineData wide;
        wide.v = {{{0, 0}, 0, 1.0, 0.2}, {{12, 0}, 0.4, 0.5, 2.0}, {{12, 9}, 0, 0, 0}, {{0, 9}, 0, 3.0, 3.0}};
        wide.closed = rng() % 2;
        r.add(wide);
        PolylineData cw;
        cw.constantWidth = rnd(0.1, 2.0);
        cw.v = {{{2, 2}, 0, 0, 0}, {{18, 3}, -0.5, 0, 0}, {{9, 14}, 0, 0, 0}};
        r.add(cw);
        r.add(CircleData{{9, 5}, 7});
        r.host.run("HATCH", {pt({rnd(2, 10), rnd(1, 8)}), ScriptItem::enter()});
        r.host.run("HATCH", {pt({rnd(2, 10), rnd(1, 8)}), ScriptItem::enter()});
        std::vector<EntityId> ids;
        r.doc.forEachEntity([&](const Entity& e) { ids.push_back(e.common.id); });
        REQUIRE(ids.size() >= 6);
        const auto go = [&](std::string_view name, std::vector<ScriptItem> items) {
            if (!r.host.run(name, items).finished)
                r.host.feed({ScriptItem::esc(), ScriptItem::esc(), ScriptItem::esc()});
        };
        const std::size_t base = r.host.undoStack().undoCount();  // setup steps stay
        std::vector<std::string> history{canonicalDump(r.doc)};
        for (int step = 0; step < 40; ++step) {
            const std::size_t a = rng() % ids.size(), b = rng() % ids.size();
            const Vec2 on{rnd(0, 20), rnd(0, 12)}, where{rnd(-5, 25), rnd(-5, 18)};
            const std::string ra = std::to_string(rnd(0.0, 5.0));
            switch (rng() % 10) {
                case 0: go("TRIM", {ScriptItem::enter(), ScriptItem::pick(ids[a], on), ScriptItem::enter()}); break;
                case 1: go("EXTEND", {ScriptItem::enter(), ScriptItem::pick(ids[a], on), ScriptItem::enter()}); break;
                case 2: go("FILLET", {"R", ra, ScriptItem::pick(ids[a], on), ScriptItem::pick(ids[b], on)}); break;
                case 3: go("CHAMFER", {"D", ra, ra, ScriptItem::pick(ids[a], on), ScriptItem::pick(ids[b], on)}); break;
                case 4: go("OFFSET", {ra, ScriptItem::pick(ids[a], on), pt(where), ScriptItem::enter()}); break;
                case 5: go("BREAK", {ScriptItem::pick(ids[a], on), pt(where)}); break;
                case 6: go("EXPLODE", {ScriptItem::select({ids[a]}), ScriptItem::enter()}); break;
                case 7: go("MIRROR", {ScriptItem::select({ids[a], ids[b]}), ScriptItem::enter(), "0,0", pt({rnd(-5, 5), rnd(-5, 5)}), "N"}); break;
                case 8: go("SCALE", {ScriptItem::select({ids[a]}), ScriptItem::enter(), pt(on), std::to_string(rnd(0.3, 2.5))}); break;
                default: go("ROTATE", {ScriptItem::select({ids[a], ids[b]}), ScriptItem::enter(), pt(on), std::to_string(rnd(-180, 180))}); break;
            }
            const std::string now = canonicalDump(r.doc);
            CHECK(finiteDump(now));
            if (step % 8 == 7) {  // what is in memory must survive saving and loading
                auto bytes = io::writeTbcadToMemory(r.doc);
                REQUIRE(bytes.ok());
                io::IoReport rep;
                auto back = io::readTbcad(std::span<const std::uint8_t>(bytes.value()), rep);
                REQUIRE(back.ok());
                CHECK(canonicalDump(*back.value()) == now);
            }
            const std::size_t depth = r.host.undoStack().undoCount() - base;
            CHECK(depth + 1 >= history.size());
            if (depth == history.size()) {
                if (now != history.back())
                    ++changed;
                history.push_back(now);
            } else {
                CHECK(depth == history.size() - 1);
                CHECK(now == history.back());
            }
        }
        for (std::size_t k = history.size() - 1; k > 0; --k) {
            CHECK(r.host.undo());
            CHECK(canonicalDump(r.doc) == history[k - 1]);
        }
    }
    MESSAGE(changed << " steps changed the drawing");
    CHECK(changed > 20);
}
