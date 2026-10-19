// Corner cases of the editing commands: COPY Array, SCALE / ROTATE by reference, ALIGN with scale.
#include <doctest/doctest.h>

#include "common.h"
#include "tbcad/blocks/attributes.h"
#include "tbcad/core/dump.h"

using namespace tbtest;
using cmd::ScriptItem;

namespace {
std::size_t lines(const Document& doc) {
    std::size_t n = 0;
    doc.forEachEntity([&](const Entity& e) { n += entityAs<LineData>(e) ? 1 : 0; });
    return n;
}
}  // namespace

TEST_CASE("COPY Array makes the copies in one undo step, with spacing or Fit") {
    {
        Fixture f;
        auto a = add(f.doc, LineData{{0, 0}, {10, 0}});
        f.host.setSelection({a});
        const std::string before = canonicalDump(f.doc);
        f.host.run("COPY", {"0,0", "Array", "4", "0,5"});
        CHECK(lines(f.doc) == 4);
        const auto undo0 = f.host.undoStack().undoCount();
        CHECK(f.host.undo());
        CHECK(f.host.undoStack().undoCount() == undo0 - 1);
        CHECK(canonicalDump(f.doc) == before);
    }
    {
        Fixture f;
        auto a = add(f.doc, LineData{{0, 0}, {10, 0}});
        f.host.setSelection({a});
        f.host.run("COPY", {"0,0", "Array", "3", "Fit", "0,20"});
        std::vector<double> ys;
        f.doc.forEachEntity([&](const Entity& e) {
            if (const auto* l = entityAs<LineData>(e))
                ys.push_back(l->a.y);
        });
        std::sort(ys.begin(), ys.end());
        REQUIRE(ys.size() == 3);
        CHECK(ys[1] == doctest::Approx(10));
        CHECK(ys[2] == doctest::Approx(20));
    }
}

TEST_CASE("COPY mOde Single makes one copy, Undo takes back the last copy, one undo step") {
    Fixture f;
    auto a = add(f.doc, LineData{{0, 0}, {10, 0}});
    f.host.setSelection({a});
    const std::string before = canonicalDump(f.doc);
    auto r = f.host.run("COPY", {"0,0", "0,5", "0,10", "0,15", "Undo", ScriptItem::enter()});
    CHECK(r.finished);
    CHECK(lines(f.doc) == 3);  // two copies stay
    CHECK(f.host.undo());
    CHECK(canonicalDump(f.doc) == before);  // the whole command is one step

    f.host.setSelection({a});
    r = f.host.run("COPY", {"mOde", "Single", "0,0", "0,5"});
    CHECK(r.finished);  // ends after one copy
    CHECK(lines(f.doc) == 2);
    CHECK(f.host.setting("copy.mode") == std::optional<std::string>("single"));
    f.host.setSelection({a});
    f.host.run("COPY", {"O", "M", "0,0", "0,7", "0,9", ScriptItem::enter()});
    CHECK(lines(f.doc) == 4);
    CHECK(f.host.logContains("Copy mode = Single"));
}

TEST_CASE("COPY Array refuses a count below 2 and keeps the drawing") {
    Fixture f;
    auto a = add(f.doc, LineData{{0, 0}, {10, 0}});
    f.host.setSelection({a});
    f.host.run("COPY", {"0,0", "Array", "1", ScriptItem::esc()});
    CHECK(lines(f.doc) == 1);
    f.host.feed({ScriptItem::esc()});
    CHECK(lines(f.doc) == 1);
}

TEST_CASE("COPY Array of a big count stays fast and exact") {
    Fixture f;
    auto a = add(f.doc, LineData{{0, 0}, {1, 0}});
    f.host.setSelection({a});
    const auto t0 = std::chrono::steady_clock::now();
    f.host.run("COPY", {"0,0", "Array", "5000", "2,0"});
    const double ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    CHECK(lines(f.doc) == 5000);
    CHECK(ms < 20000);
    double maxX = 0;
    f.doc.forEachEntity([&](const Entity& e) {
        if (const auto* l = entityAs<LineData>(e))
            maxX = std::max(maxX, l->a.x);
    });
    CHECK(maxX == doctest::Approx(2.0 * 4999));
    CHECK(f.host.undo());
    CHECK(lines(f.doc) == 1);
}

TEST_CASE("SCALE by reference: equal lengths change nothing, zero new length is refused") {
    {
        Fixture f;
        auto a = add(f.doc, LineData{{0, 0}, {10, 0}});
        f.host.setSelection({a});
        const std::string before = canonicalDump(f.doc);
        f.host.run("SCALE", {"0,0", "Reference", "10", "10"});
        CHECK(near(get<LineData>(f.doc, a).b, {10, 0}));
        (void)before;
    }
    {
        Fixture f;
        auto a = add(f.doc, LineData{{0, 0}, {10, 0}});
        f.host.setSelection({a});
        const std::string before = canonicalDump(f.doc);
        f.host.run("SCALE", {"0,0", "Reference", "10", "0", ScriptItem::esc()});
        CHECK(canonicalDump(f.doc) == before);
    }
    {
        // Reference picked with points: 10 long becomes the distance 0,0 to 0,30.
        Fixture f;
        auto a = add(f.doc, LineData{{0, 0}, {10, 0}});
        f.host.setSelection({a});
        f.host.run("SCALE", {"0,0", "Reference", "0,0", "10,0", "0,30"});
        CHECK(near(get<LineData>(f.doc, a).b, {30, 0}));
    }
}

TEST_CASE("ROTATE by reference works with negative, wrapped and equal angles, and with Copy") {
    {
        Fixture f;
        auto a = add(f.doc, LineData{{10, 0}, {20, 0}});
        f.host.setSelection({a});
        f.host.run("ROTATE", {"0,0", "Reference", "350", "10"});  // 20 degrees
        const double c = std::cos(20 * kPi / 180), s = std::sin(20 * kPi / 180);
        CHECK(near(get<LineData>(f.doc, a).b, {20 * c, 20 * s}, 1e-9));
    }
    {
        Fixture f;
        auto a = add(f.doc, LineData{{10, 0}, {20, 0}});
        f.host.setSelection({a});
        f.host.run("ROTATE", {"0,0", "Reference", "40", "40"});
        CHECK(near(get<LineData>(f.doc, a).b, {20, 0}, 1e-9));
    }
    {
        Fixture f;
        auto a = add(f.doc, LineData{{10, 0}, {20, 0}});
        f.host.setSelection({a});
        f.host.run("ROTATE", {"0,0", "Copy", "Reference", "0", "90"});
        CHECK(lines(f.doc) == 2);
        CHECK(near(get<LineData>(f.doc, a).b, {20, 0}));
        CHECK(f.host.undo());
        CHECK(lines(f.doc) == 1);
    }
}

TEST_CASE("ALIGN with two point pairs rotates, and with Yes also scales") {
    {
        Fixture f;
        auto a = add(f.doc, LineData{{0, 0}, {10, 0}});
        f.host.setSelection({a});
        f.host.run("ALIGN", {"0,0", "5,5", "10,0", "5,25", "N"});
        // The source direction is +x; the destination direction is +y, no scaling.
        CHECK(near(get<LineData>(f.doc, a).a, {5, 5}, 1e-9));
        CHECK(near(get<LineData>(f.doc, a).b, {5, 15}, 1e-9));
    }
    {
        Fixture f;
        auto a = add(f.doc, LineData{{0, 0}, {10, 0}});
        f.host.setSelection({a});
        f.host.run("ALIGN", {"0,0", "5,5", "10,0", "5,25", "Y"});
        CHECK(near(get<LineData>(f.doc, a).a, {5, 5}, 1e-9));
        CHECK(near(get<LineData>(f.doc, a).b, {5, 25}, 1e-9));
    }
    {
        // Text scales with it.
        Fixture f;
        TextData t;
        t.utf8 = "A";
        t.height = 2;
        t.position = {0, 0};
        t.alignPoint = t.position;
        auto a = add(f.doc, t);
        f.host.setSelection({a});
        f.host.run("ALIGN", {"0,0", "0,0", "10,0", "30,0", "Y"});
        CHECK(get<TextData>(f.doc, a).height == doctest::Approx(6));
    }
}

TEST_CASE("ALIGN with coincident points is refused without touching the drawing") {
    Fixture f;
    auto a = add(f.doc, LineData{{0, 0}, {10, 0}});
    f.host.setSelection({a});
    const std::string before = canonicalDump(f.doc);
    f.host.run("ALIGN", {"5,5", "0,0", "5,5", "20,0", "Y"});
    CHECK(canonicalDump(f.doc) == before);
    CHECK_FALSE(f.host.runner().active());
}

TEST_CASE("A zero displacement copies and moves nothing") {
    {
        Fixture f;
        auto a = add(f.doc, LineData{{0, 0}, {10, 0}});
        f.host.setSelection({a});
        f.host.run("COPY", {"5,5", "5,5", ScriptItem::esc()});  // second point on the base
        CHECK(lines(f.doc) == 1);
    }
    {
        Fixture f;
        auto a = add(f.doc, LineData{{0, 0}, {10, 0}});
        f.host.setSelection({a});
        f.host.run("COPY", {"0,0", ScriptItem::enter()});  // Enter: base point is the vector, zero
        CHECK(lines(f.doc) == 1);
    }
    {
        Fixture f;
        auto a = add(f.doc, LineData{{0, 0}, {10, 0}});
        f.host.setSelection({a});
        f.host.run("COPY", {"Displacement", "0,0"});
        CHECK(lines(f.doc) == 1);
    }
    {
        Fixture f;
        auto a = add(f.doc, LineData{{0, 0}, {10, 0}});
        f.host.setSelection({a});
        const auto undo0 = f.host.undoStack().undoCount();
        f.host.run("MOVE", {"Displacement", "0,0"});
        f.host.run("MOVE", {"3,3", "3,3"});
        CHECK(f.host.undoStack().undoCount() == undo0);
    }
    {
        Fixture f;  // Array with the second point on the base asks again, then accepts a real one
        auto a = add(f.doc, LineData{{0, 0}, {10, 0}});
        f.host.setSelection({a});
        f.host.run("COPY", {"0,0", "Array", "3", "0,0", "0,5"});
        CHECK(lines(f.doc) == 3);
    }
}

TEST_CASE("ROTATE and SCALE: Copy together with Reference, one undo step, original untouched") {
    {
        Fixture f;
        auto a = add(f.doc, LineData{{10, 0}, {20, 0}});
        f.host.setSelection({a});
        const std::string before = canonicalDump(f.doc);
        // Copy first, then Reference: the copy turns by new - old = 90 degrees.
        f.host.run("ROTATE", {"0,0", "Copy", "Reference", "0", "90"});
        CHECK(f.doc.modelSpaceCount() == 2);
        CHECK(near(get<LineData>(f.doc, a).b, {20, 0}));
        bool found = false;
        for (EntityId id : f.doc.modelSpace())
            found |= near(get<LineData>(f.doc, id).b, {0, 20});
        CHECK(found);
        CHECK(f.host.undo());
        CHECK(canonicalDump(f.doc) == before);
        f.host.setSelection({a});
        f.host.run("RO", {"0,0", "C", "C", "180"});  // asking twice keeps the copy mode
        CHECK(f.doc.modelSpaceCount() == 2);
    }
    {
        Fixture f;
        auto a = add(f.doc, LineData{{0, 0}, {10, 0}});
        f.host.setSelection({a});
        const std::string before = canonicalDump(f.doc);
        f.host.run("SCALE", {"0,0", "Copy", "Reference", "10", "25"});
        CHECK(f.doc.modelSpaceCount() == 2);
        CHECK(near(get<LineData>(f.doc, a).b, {10, 0}));
        bool found = false;
        for (EntityId id : f.doc.modelSpace())
            found |= near(get<LineData>(f.doc, id).b, {25, 0});
        CHECK(found);
        CHECK(f.host.undo());
        CHECK(canonicalDump(f.doc) == before);
    }
}

namespace {
EntityId linearDim(Fixture& f) {
    f.host.run("DIMLINEAR", {"0,0", "10,0", "5,5"});
    EntityId id{};
    f.doc.forEachEntity([&](const Entity& e) {
        if (entityAs<DimensionData>(e))
            id = e.common.id;
    });
    return id;
}
}  // namespace

TEST_CASE("SCALE by reference and ALIGN with scale keep a dimension measuring what it shows") {
    {
        Fixture f;
        const EntityId d = linearDim(f);
        REQUIRE(d);
        f.host.setSelection({d});
        f.host.run("SCALE", {"0,0", "Reference", "10", "25"});
        CHECK(get<DimensionData>(f.doc, d).measurement == doctest::Approx(25));
        CHECK(near(get<DimensionData>(f.doc, d).defPts[1], {25, 0}));
        CHECK(f.host.undo());
        CHECK(get<DimensionData>(f.doc, d).measurement == doctest::Approx(10));
    }
    {
        // ALIGN 0,0-10,0 onto 0,0-0,30 with scaling: rotated a quarter turn, three times longer.
        Fixture f;
        const EntityId d = linearDim(f);
        REQUIRE(d);
        f.host.setSelection({d});
        f.host.run("ALIGN", {"0,0", "0,0", "10,0", "0,30", "Y"});
        CHECK(get<DimensionData>(f.doc, d).measurement == doctest::Approx(30));
        CHECK(near(get<DimensionData>(f.doc, d).defPts[1], {0, 30}));
    }
}

TEST_CASE("ROTATE by reference turns a dimension and a multileader without changing sizes") {
    Fixture f;
    const EntityId d = linearDim(f);
    REQUIRE(d);
    MLeaderData ml;
    ml.lines = {{{0, 20}, {10, 30}}};
    ml.landing = {10, 30};
    ml.contentPos = {15, 30};
    ml.utf8 = "A";
    const EntityId m = add(f.doc, Indirect<MLeaderData>(ml));
    f.host.setSelection({d, m});
    // Reference 0 degrees, new 90 degrees: a quarter turn about the origin.
    f.host.run("ROTATE", {"0,0", "Reference", "0", "90"});
    const auto& dd = get<DimensionData>(f.doc, d);
    CHECK(dd.measurement == doctest::Approx(10));
    CHECK(near(dd.defPts[1], {0, 10}, 1e-9));
    CHECK(dd.rotation == doctest::Approx(1.5707963267948966));
    const auto& mm = get<MLeaderData>(f.doc, m);
    CHECK(near(mm.landing, {-30, 10}, 1e-9));
    CHECK(near(mm.contentPos, {-30, 15}, 1e-9));
    CHECK(near(mm.lines[0][0], {-20, 0}, 1e-9));
}

TEST_CASE("ROTATE and SCALE turn a block with attributes and its attribute offsets together") {
    Fixture f;
    auto t = f.doc.begin("block");
    BlockId b = t.addBlock(BlockDef{.name = "Tagged"}).value();
    TextData def;
    def.position = {4, 2};
    def.alignPoint = def.position;
    def.height = 1;
    def.utf8 = "NAME";
    def.attdef = AttDefInfo{"NAME", "", "x", false, false, false, false};
    Entity de;
    de.data = Indirect<TextData>(def);
    REQUIRE(t.add(std::move(de), b).ok());
    t.commit();
    InsertData in{b, {10, 0}, {1, 1}, 0};
    in.attribs = blocks::defaultAttribs(f.doc, b);
    const EntityId id = add(f.doc, in);
    f.host.setSelection({id});
    auto anchor = [&] { return blocks::attribHandles(f.doc, *f.doc.find(id)).at(0).world; };
    const Vec2 a0 = anchor();
    // Quarter turn about the insert point: the attribute anchor turns with it.
    f.host.run("ROTATE", {"10,0", "90"});
    CHECK(near(anchor(), {10 - (a0.y - 0), (a0.x - 10)}, 1e-9));
    CHECK(get<InsertData>(f.doc, id).rotation == doctest::Approx(1.5707963267948966));
    // Scale by reference about the insert point: the anchor distance doubles.
    const Vec2 a1 = anchor();
    f.host.setSelection({id});
    f.host.run("SCALE", {"10,0", "Reference", "5", "10"});
    CHECK(near(anchor(), {10 + 2 * (a1.x - 10), 2 * a1.y}, 1e-9));
    CHECK(get<InsertData>(f.doc, id).scale.x == doctest::Approx(2));
    CHECK(f.host.undo());
    CHECK(f.host.undo());
    CHECK(near(anchor(), a0, 1e-9));
}

TEST_CASE("STRETCH of one end of a dimension updates its measurement") {
    Fixture f;
    const EntityId d = linearDim(f);
    REQUIRE(d);
    // Crossing window around the second extension origin only.
    f.host.run("STRETCH", {"8,-2", "12,2", "10,0", "16,0"});
    CHECK(get<DimensionData>(f.doc, d).measurement == doctest::Approx(16));
    CHECK(near(get<DimensionData>(f.doc, d).defPts[0], {0, 0}));
    CHECK(f.host.undo());
    CHECK(get<DimensionData>(f.doc, d).measurement == doctest::Approx(10));
}

TEST_CASE("STRETCH with a whole dimension in the window moves it, text included") {
    Fixture f;
    const EntityId d = linearDim(f);
    REQUIRE(d);
    const Vec2 text0 = get<DimensionData>(f.doc, d).textPos;
    f.host.run("STRETCH", {"-5,-5", "40,20", "0,0", "0,7"});
    CHECK(get<DimensionData>(f.doc, d).measurement == doctest::Approx(10));
    CHECK(near(get<DimensionData>(f.doc, d).defPts[0], {0, 7}));
    CHECK(near(get<DimensionData>(f.doc, d).textPos, text0 + Vec2{0, 7}, 1e-6));
}

TEST_CASE("an xref picture with a clip boundary moves, turns, scales and mirrors with it") {
    struct Op {
        const char* name;
        std::vector<ScriptItem> in;
        Transform2 t;
    };
    const Op ops[] = {
        {"MOVE", {"0,0", "13,-7"}, Transform2::translate({13, -7})},
        {"ROTATE", {"5,5", "30"}, Transform2::rotate(30 * 3.14159265358979323846 / 180, {5, 5})},
        {"SCALE", {"0,0", "2.5"}, Transform2::scale(2.5, 2.5)},
        {"MIRROR", {"0,0", "10,0", "Y"}, Transform2::scale(1, -1)},
    };
    for (const Op& op : ops) {
        CAPTURE(std::string(op.name));
        Fixture f;
        BlockId blk{};
        {
            auto tx = f.doc.begin("image");
            blk = tx.addBlock(BlockDef{.name = "IMG", .xrefPath = "a.png"}).value();
            PolylineData frame;
            frame.closed = true;
            for (Vec2 p : {Vec2{0, 0}, Vec2{40, 0}, Vec2{40, 20}, Vec2{0, 20}})
                frame.v.push_back({p, 0, 0, 0});
            Entity fe;
            fe.data = std::move(frame);
            REQUIRE(tx.add(std::move(fe), blk).ok());
            tx.commit();
        }
        InsertData in{blk, {10, 20}, {0.5, 0.5}, 0.2};
        in.clip = {{5, 2}, {35, 2}, {35, 18}, {5, 18}};
        const EntityId id = add(f.doc, in);
        auto world = [&](const InsertData& d, Vec2 p) {
            const double c = std::cos(d.rotation), s = std::sin(d.rotation);
            return d.position + Vec2{c * d.scale.x * p.x - s * d.scale.y * p.y,
                                     s * d.scale.x * p.x + c * d.scale.y * p.y};
        };
        std::vector<Vec2> expected;
        for (Vec2 p : in.clip)
            expected.push_back(op.t.apply(world(in, p)));
        f.host.setSelection({id});
        f.host.run(op.name, op.in);
        const InsertData& out = get<InsertData>(f.doc, id);
        REQUIRE(out.clip.size() == 4);
        for (std::size_t i = 0; i < 4; ++i)
            CHECK(near(world(out, out.clip[i]), expected[i], 1e-9));
        CHECK(out.clipOn);
        CHECK(f.host.undo());
        CHECK(near(get<InsertData>(f.doc, id).position, in.position));
        CHECK(get<InsertData>(f.doc, id).clip == in.clip);
    }
}

TEST_CASE("SCALE and ROTATE Reference: Points gives the new length or angle by two points") {
    {
        Fixture f;
        auto a = add(f.doc, LineData{{0, 0}, {10, 0}});
        f.host.setSelection({a});
        // reference 10, new length = distance 100,100 to 100,125
        REQUIRE(f.host.run("SCALE", {"0,0", "Reference", "10", "Points", "100,100", "100,125"})
                    .finished);
        CHECK(near(get<LineData>(f.doc, a).b, {25, 0}));
        CHECK(f.host.undo());
        CHECK(near(get<LineData>(f.doc, a).b, {10, 0}));
    }
    {
        Fixture f;
        auto a = add(f.doc, LineData{{0, 0}, {10, 0}});
        f.host.setSelection({a});
        // reference picked as the line itself, new direction from 5,5 to 5,15 (90 degrees)
        REQUIRE(f.host.run("ROTATE", {"0,0", "R", "0,0", "10,0", "P", "5,5", "5,15"}).finished);
        CHECK(near(get<LineData>(f.doc, a).b, {0, 10}, 1e-9));
    }
    {
        // Esc in the middle of Points changes nothing; equal points are refused
        Fixture f;
        auto a = add(f.doc, LineData{{0, 0}, {10, 0}});
        f.host.setSelection({a});
        const std::string before = canonicalDump(f.doc);
        f.host.run("SCALE", {"0,0", "R", "10", "P", "1,1", ScriptItem::esc()});
        CHECK(canonicalDump(f.doc) == before);
        f.host.setSelection({a});
        f.host.run("SCALE", {"0,0", "R", "10", "P", "1,1", "1,1"});
        CHECK(canonicalDump(f.doc) == before);
        f.host.setSelection({a});
        f.host.run("ROTATE", {"0,0", "R", "0", "P", "1,1", "1,1"});
        CHECK(canonicalDump(f.doc) == before);
    }
}
