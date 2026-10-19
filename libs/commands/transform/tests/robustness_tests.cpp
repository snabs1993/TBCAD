#include <doctest/doctest.h>

#include <cctype>
#include <string>

#include "common.h"
#include "tbcad/blocks/attributes.h"
#include "tbcad/core/dump.h"
#include "tbcad/geom/geom.h"
#include "tbcad/transform/grip_menu.h"
#include "tbcad/transform/grips.h"

using namespace tbtest;

namespace {

bool hasNonFinite(const std::string& dump) {
    for (const char* bad : {"nan", "inf"}) {
        std::string low = dump;
        for (auto& c : low)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (low.find(bad) != std::string::npos)
            return true;
    }
    return false;
}

void addSamples(Document& doc, std::vector<EntityId>& ids) {
    ids.push_back(add(doc, LineData{{0, 0}, {10, 5}}));
    ids.push_back(add(doc, CircleData{{20, 20}, 4}));
    ids.push_back(add(doc, ArcData{{0, 30}, 6, 0.3, 2.5}));
    PolylineData pl;
    pl.v = {{{0, 0}, 0.5, 0, 0}, {{10, 0}, 0, 1, 2}, {{10, 10}, -0.3, 0, 0}};
    pl.closed = true;
    ids.push_back(add(doc, pl));
    ids.push_back(add(doc, PointData{{3, 3}}));
    EllipseData el;
    el.center = {5, 5};
    el.majorAxis = {6, 2};
    el.ratio = 0.5;
    ids.push_back(add(doc, Indirect<EllipseData>(el)));
    TextData tx;
    tx.position = {1, 2};
    tx.alignPoint = {1, 2};
    tx.utf8 = "Hello";
    ids.push_back(add(doc, Indirect<TextData>(tx)));
    RayData ray;
    ray.origin = {2, 2};
    ray.dir = {1, 1};
    ids.push_back(add(doc, Indirect<RayData>(ray)));
    MlineData ml;
    ml.v = {{0, 0}, {5, 0}, {5, 5}, {9, 5}};
    ml.cuts = {{0, 0.3, 0.5, 0, 255}, {2, 0.2, 0.6, 0, 255}};
    ids.push_back(add(doc, Indirect<MlineData>(ml)));
    TableData tb;
    tb.origin = {30, 30};
    tb.colWidths = {10, 12, 8};
    tb.rowHeights = {4, 5, 6};
    tb.cells.resize(9);
    ids.push_back(add(doc, Indirect<TableData>(tb)));
    MTextData mt;
    mt.position = {40, 10};
    mt.width = 25;
    mt.utf8 = "wrapped text for the width grip";
    ids.push_back(add(doc, Indirect<MTextData>(mt)));
    LeaderData ld;
    ld.vertices = {{0, 0}, {4, 4}};
    ld.utf8 = "note";
    ld.textPos = {5, 5};
    ids.push_back(add(doc, Indirect<LeaderData>(ld)));

    // Blocks with their own grips: attributes, a picture frame, dynamic parameters.
    auto block = [&](const char* name, std::string xref) {
        auto t = doc.begin("block");
        BlockId b = t.addBlock(BlockDef{.name = name, .xrefPath = std::move(xref)}).value();
        t.commit();
        return b;
    };
    auto put = [&](BlockId b, EntityData data) {
        auto t = doc.begin("content");
        Entity e;
        e.data = std::move(data);
        EntityId id = t.add(std::move(e), b).value();
        t.commit();
        return id;
    };
    const BlockId tagged = block("Tagged", "");
    TextData def;
    def.position = {0, 2};
    def.height = 1;
    def.utf8 = "NAME";
    def.attdef = AttDefInfo{"NAME", "", "x", false, false, false, false};
    put(tagged, Indirect<TextData>(def));
    InsertData withTag{tagged, {50, 0}, {1.5, 1.5}, 0.4};
    withTag.attribs = blocks::defaultAttribs(doc, tagged);
    ids.push_back(add(doc, withTag));

    const BlockId pic = block("Picture", "photo.png");
    PolylineData frame;
    frame.closed = true;
    for (Vec2 p : {Vec2{0, 0}, Vec2{40, 0}, Vec2{40, 20}, Vec2{0, 20}})
        frame.v.push_back({p, 0, 0, 0});
    put(pic, frame);
    ids.push_back(add(doc, InsertData{pic, {60, 40}, {0.5, 0.5}, 0.2}));

    const BlockId dyn = block("Dyn", "");
    const EntityId bar = put(dyn, LineData{{0, 0}, {10, 0}});
    {
        auto t = doc.begin("dyn");
        DynamicDef dd;
        DynParam w;
        w.name = "Length";
        w.base = {0, 0};
        w.end = {10, 0};
        dd.params.push_back(w);
        DynParam fl;
        fl.kind = DynParam::Kind::Flip;
        fl.name = "Flip";
        fl.base = {5, 0};
        fl.end = {5, 5};
        dd.params.push_back(fl);
        DynParam lk;
        lk.kind = DynParam::Kind::Lookup;
        lk.name = "Size";
        lk.base = {0, 4};
        lk.states = {"S", "L"};
        lk.stateObjects = {{}, {}};
        lk.lookupParams = {0};
        lk.lookupValues = {{10}, {25}};
        dd.params.push_back(lk);
        DynAction st;
        st.kind = DynAction::Kind::Stretch;
        st.frameMin = {9, -1};
        st.frameMax = {11, 1};
        st.objects = {bar};
        dd.actions.push_back(st);
        REQUIRE(t.modifyBlock(dyn, [&](BlockDef& d) { d.dynamic = std::move(dd); }).ok());
        t.commit();
    }
    ids.push_back(add(doc, InsertData{dyn, {80, 0}, {1, 1}, 0}));
}

}  // namespace

TEST_CASE("transforms keep every entity kind finite and undo back to the exact original") {
    struct Case {
        const char* cmd;
        std::vector<cmd::ScriptItem> in;
    };
    const Case cases[] = {
        {"MOVE", {"0,0", "13.5,-7.25"}},   {"MOVE", {"0,0", "1e6,-1e6"}},
        {"ROTATE", {"3,4", "37"}},         {"ROTATE", {"0,0", "180"}},
        {"SCALE", {"1,1", "2.5"}},         {"SCALE", {"0,0", "0.001"}},
        {"MIRROR", {"0,0", "10,3", "No"}}, {"MIRROR", {"0,0", "0,10", "Yes"}},
    };
    for (const auto& c : cases) {
        CAPTURE(c.cmd);
        Fixture f;
        std::vector<EntityId> ids;
        addSamples(f.doc, ids);
        const std::string before = canonicalDump(f.doc);
        f.host.setSelection(ids);
        auto r = f.host.run(c.cmd, c.in);
        CHECK(r.finished);
        const std::string after = canonicalDump(f.doc);
        CHECK_FALSE(hasNonFinite(after));
        CHECK(after != before);
        CHECK(f.host.undo());
        CHECK(canonicalDump(f.doc) == before);
        CHECK(f.host.redo());
        CHECK(canonicalDump(f.doc) == after);
    }
}

TEST_CASE("transforms of entities owned by a block (a layout sheet) undo to the exact original") {
    for (const char* cmdName : {"MOVE", "ROTATE", "SCALE", "MIRROR"}) {
        CAPTURE(cmdName);
        Fixture f;
        {
            auto tx = f.doc.begin("sheet");
            addOwner() = tx.addBlock(BlockDef{.name = "Sheet"}).value();
            tx.commit();
        }
        std::vector<EntityId> ids;
        addSamples(f.doc, ids);
        addOwner() = BlockId{};
        const std::string before = canonicalDump(f.doc);
        f.host.setSelection(ids);
        const std::string name = cmdName;
        std::vector<cmd::ScriptItem> in =
            name == "MOVE"     ? std::vector<cmd::ScriptItem>{"0,0", "5,7"}
            : name == "ROTATE" ? std::vector<cmd::ScriptItem>{"3,4", "33"}
            : name == "SCALE"  ? std::vector<cmd::ScriptItem>{"1,1", "1.5"}
                               : std::vector<cmd::ScriptItem>{"0,0", "10,3", "No"};
        auto r = f.host.run(cmdName, in);
        CHECK(r.finished);
        const std::string after = canonicalDump(f.doc);
        CHECK_FALSE(hasNonFinite(after));
        CHECK(after != before);
        CHECK(f.host.undo());
        CHECK(canonicalDump(f.doc) == before);
        CHECK(f.host.redo());
        CHECK(canonicalDump(f.doc) == after);
    }
}

TEST_CASE("rotating by 360 degrees and moving there and back restore the geometry") {
    Fixture f;
    std::vector<EntityId> ids;
    addSamples(f.doc, ids);
    f.host.setSelection(ids);
    f.host.run("MOVE", {"0,0", "100,-40"});
    f.host.setSelection(ids);
    f.host.run("MOVE", {"0,0", "-100,40"});
    const auto& l = get<LineData>(f.doc, ids[0]);
    CHECK(near(l.a, {0, 0}, 1e-9));
    CHECK(near(l.b, {10, 5}, 1e-9));
    f.host.setSelection(ids);
    f.host.run("ROTATE", {"7,3", "360"});
    CHECK(near(get<LineData>(f.doc, ids[0]).b, {10, 5}, 1e-9));
    CHECK(near(get<CircleData>(f.doc, ids[1]).center, {20, 20}, 1e-9));
}

TEST_CASE("scaling by zero or a negative factor is refused without touching the drawing") {
    Fixture f;
    std::vector<EntityId> ids;
    addSamples(f.doc, ids);
    const std::string before = canonicalDump(f.doc);
    f.host.setSelection(ids);
    f.host.run("SCALE", {"0,0", "0"});
    CHECK(canonicalDump(f.doc) == before);
    f.host.setSelection(ids);
    f.host.run("SCALE", {"0,0", "-2"});
    CHECK(canonicalDump(f.doc) == before);
}

TEST_CASE("dragging any grip of any entity kind stays finite and undoes exactly") {
    Fixture f0;
    std::vector<EntityId> ids0;
    addSamples(f0.doc, ids0);
    std::size_t gripCount = 0;
    for (EntityId id : ids0)
        gripCount += tbcad::transform::collectGrips(f0.doc, std::vector<EntityId>{id}).size();
    CHECK(gripCount > 20);
    for (std::size_t k = 0; k < ids0.size(); ++k) {
        const std::size_t n =
            tbcad::transform::collectGrips(f0.doc, std::vector<EntityId>{ids0[k]}).size();
        for (std::size_t g = 0; g < n; ++g) {
            CAPTURE(k);
            CAPTURE(g);
            Fixture f;
            std::vector<EntityId> ids;
            addSamples(f.doc, ids);
            const auto gs = tbcad::transform::collectGrips(f.doc, std::vector<EntityId>{ids[k]});
            REQUIRE(g < gs.size());
            const std::string before = canonicalDump(f.doc);
            f.host.setSelection({ids[k]});
            const Vec2 from = gs[g].pos;
            const std::string src = std::to_string(from.x) + "," + std::to_string(from.y);
            const std::string dst =
                std::to_string(from.x + 3.5) + "," + std::to_string(from.y - 2.25);
            f.host.run("GRIPEDIT", {src, dst});
            const std::string after = canonicalDump(f.doc);
            CHECK_FALSE(hasNonFinite(after));
            if (after != before) {
                CHECK(f.host.undo());
                CHECK(canonicalDump(f.doc) == before);
            }
        }
    }
}

TEST_CASE("STRETCH over a window that catches parts of every entity kind undoes exactly") {
    const char* windows[][2] = {{"-5,-5", "6,6"}, {"4,-5", "25,40"}, {"-100,-100", "100,100"}};
    for (const auto& w : windows) {
        CAPTURE(w[0]);
        Fixture f;
        std::vector<EntityId> ids;
        addSamples(f.doc, ids);
        const std::string before = canonicalDump(f.doc);
        f.host.run("STRETCH", {w[0], w[1], "0,0", "7.5,-3"});
        const std::string after = canonicalDump(f.doc);
        CHECK_FALSE(hasNonFinite(after));
        if (after != before) {
            CHECK(f.host.undo());
            CHECK(canonicalDump(f.doc) == before);
            CHECK(f.host.redo());
            CHECK(canonicalDump(f.doc) == after);
        }
    }
}

TEST_CASE("every grip menu action on every grip stays finite and undoes exactly") {
    using tbcad::transform::GripAction;
    auto samples = [](Document& doc, std::vector<EntityId>& ids) {
        ids.push_back(add(doc, LineData{{0, 0}, {10, 5}}));
        ids.push_back(add(doc, CircleData{{20, 20}, 4}));
        ids.push_back(add(doc, ArcData{{0, 30}, 6, 0.3, 2.5}));
        PolylineData pl;
        pl.v = {{{0, 0}, 0.5, 0, 0}, {{10, 0}, 0, 1, 2}, {{10, 10}, -0.3, 0, 0}};
        pl.closed = true;
        ids.push_back(add(doc, pl));
        SplineData sp;
        sp.fit = {{0, 0}, {5, 4}, {10, 0}, {15, 4}};
        if (auto r = geom::interpolateSpline(sp.fit, {})) {
            sp.degree = r.value().degree;
            sp.knots = r.value().knots;
            sp.control = r.value().control;
        }
        ids.push_back(add(doc, Indirect<SplineData>(sp)));
        LeaderData ld;
        ld.vertices = {{0, 0}, {4, 4}, {8, 4}};
        ld.textPos = {9, 4};
        ids.push_back(add(doc, Indirect<LeaderData>(ld)));
        HatchLoop loop;
        loop.edges = {HatchArcEdge{{0, 0}, 5, 0, kPi, true}, HatchLineEdge{{-5, 0}, {5, 0}}};
        HatchData h;
        h.loops.push_back(loop);
        ids.push_back(add(doc, Indirect<HatchData>(h)));
    };
    Fixture f0;
    std::vector<EntityId> ids0;
    samples(f0.doc, ids0);
    std::size_t tried = 0;
    for (std::size_t k = 0; k < ids0.size(); ++k)
        for (const auto& g0 :
             tbcad::transform::collectGrips(f0.doc, std::vector<EntityId>{ids0[k]}))
            for (GripAction a : tbcad::transform::gripActions(f0.doc, g0)) {
                if (a == GripAction::Stretch)
                    continue;
                CAPTURE(k);
                CAPTURE(g0.index);
                CAPTURE(tbcad::transform::gripActionKeyword(a));
                Fixture f;
                std::vector<EntityId> ids;
                samples(f.doc, ids);
                const auto gs =
                    tbcad::transform::collectGrips(f.doc, std::vector<EntityId>{ids[k]});
                const auto gi = std::find_if(gs.begin(), gs.end(),
                                             [&](const auto& g) { return g.index == g0.index; });
                REQUIRE(gi != gs.end());
                const std::string before = canonicalDump(f.doc);
                f.host.setSelection({ids[k]});
                std::vector<cmd::ScriptItem> in{
                    std::to_string(gi->pos.x) + "," + std::to_string(gi->pos.y),
                    tbcad::transform::gripActionKeyword(a)};
                if (tbcad::transform::gripActionNeedsPoint(a))
                    in.push_back(std::to_string(gi->pos.x + 1.5) + "," +
                                 std::to_string(gi->pos.y + 2.5));
                f.host.run("GRIPEDIT", in);
                if (f.host.runner().active())
                    f.host.feed({cmd::ScriptItem::esc()});
                const std::string after = canonicalDump(f.doc);
                CHECK_FALSE(hasNonFinite(after));
                ++tried;
                if (after != before) {
                    CHECK(f.host.undo());
                    CHECK(canonicalDump(f.doc) == before);
                }
            }
    CHECK(tried > 10);
}
