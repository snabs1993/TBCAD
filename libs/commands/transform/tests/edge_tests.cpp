// Corner cases of the editing commands: COPY Array, SCALE / ROTATE by reference, ALIGN with scale.
#include <doctest/doctest.h>

#include "common.h"
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
