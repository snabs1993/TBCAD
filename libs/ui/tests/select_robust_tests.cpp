// Selection on a big drawing that also has nested blocks and locked, frozen and switched-off
// layers: every method (window, crossing, WPolygon, CPolygon, fence, All, Previous, QSELECT and
// FILTER) is checked against a brute-force count worked out from the way the drawing was built,
// and timed. Timings are asserted only in optimised builds.
#include <doctest/doctest.h>

#include <chrono>
#include <cstdio>
#include <set>

#include "tbcad/blocks/tbcad_blocks.h"
#include "tbcad/select/select.h"
#include "tbcad/ui/app_core.h"
#include "tbcad/ui/selection.h"
#include "test_support.h"

using namespace tbcad;
using namespace tbcad::ui;
using namespace tbcad::ui::test;

namespace {

double msSince(std::chrono::steady_clock::time_point t) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t).count();
}

constexpr int kCols = 700;
constexpr int kLines = 392000;  // 560 rows of 700
constexpr double kPitch = 30.0;
constexpr int kInsertCols = 100;
constexpr int kInserts = 3000;
constexpr double kInsertY0 = 100000.0;  // the block inserts live far above the lines
constexpr double kInsertPitch = 50.0;

// Layers by index % 10: 0..6 plain, 7 locked, 8 frozen, 9 switched off.
bool pickableSlot(int i) {
    return i % 10 < 7;
}

struct Big {
    std::unique_ptr<cmd::Registry> registry = makeRegistry();
    std::unique_ptr<AppCore> core;
    std::vector<LayerId> layers;
    Big() {
        blocks::registerInsertDisplay();
        select::registerCommands(*registry);
        auto dir = tempDir("selrobust");
        core = std::make_unique<AppCore>(*registry, platform::AppPaths{dir / "cfg", dir / "data"});
        core->setViewportSize(1400, 900);
        Document& doc = core->document();
        const char* names[] = {"P0", "P1", "P2", "P3", "P4", "P5", "P6", "LOCK", "FROZEN", "OFF"};
        for (const char* n : names)
            layers.push_back(addLayer(doc, n));
        modifyLayer(doc, layers[7], [](Layer& l) { l.locked = true; });
        modifyLayer(doc, layers[8], [](Layer& l) { l.frozen = true; });
        modifyLayer(doc, layers[9], [](Layer& l) { l.on = false; });
        auto tx = doc.begin("gen");
        BlockId inner = tx.addBlock(BlockDef{.name = "INNER"}).value();
        (void)tx.add(Entity{{}, LineData{{0, 0}, {10, 5}}}, inner);
        (void)tx.add(Entity{{}, LineData{{0, 5}, {10, 10}}}, inner);
        BlockId outer = tx.addBlock(BlockDef{.name = "OUTER"}).value();
        for (double x : {0.0, 20.0}) {
            Entity e;
            e.data = Indirect<InsertData>(InsertData{inner, {x, 0}, {1, 1}, 0});
            (void)tx.add(std::move(e), outer);
        }
        for (int i = 0; i < kLines; ++i) {
            const double x = (i % kCols) * kPitch, y = (i / kCols) * kPitch;
            Entity e{{}, LineData{{x, y}, {x + 20, y + 10}}};
            e.common.layer = layers[i % 10];
            (void)tx.add(std::move(e));
        }
        for (int j = 0; j < kInserts; ++j) {
            Entity e;
            e.data = Indirect<InsertData>(InsertData{
                outer,
                {(j % kInsertCols) * kInsertPitch, kInsertY0 + (j / kInsertCols) * kInsertPitch},
                {1, 1},
                0});
            e.common.layer = layers[j % 10];
            (void)tx.add(std::move(e));
        }
        tx.commit();
        core->zoomExtents();
        core->updateFrame();
    }
    // Window around cells c0..c1 x r0..r1 of the line grid, nothing else touched.
    static BBox2 cellBox(int c0, int c1, int r0, int r1) {
        return BBox2{{c0 * kPitch - 5, r0 * kPitch - 5}, {c1 * kPitch + 25, r1 * kPitch + 15}};
    }
    std::vector<EntityId> lineIds() const {
        std::vector<EntityId> v;
        core->document().forEachEntity([&](const Entity& e) {
            if (entityAs<LineData>(e) && core->document().ownerBlock(e.common.id) == BlockId{})
                v.push_back(e.common.id);
        });
        std::sort(v.begin(), v.end());
        return v;
    }
};

Big& big() {
    static Big b;
    return b;
}

// Which index the i-th model-space line (ascending id) has: ids were added in order.
std::size_t expectedLines(int c0, int c1, int r0, int r1) {
    std::size_t n = 0;
    for (int r = r0; r <= r1; ++r)
        for (int c = c0; c <= c1; ++c)
            if (pickableSlot(r * kCols + c))
                ++n;
    return n;
}

std::set<EntityId> asSet(const std::vector<EntityId>& v) {
    return {v.begin(), v.end()};
}

}  // namespace

TEST_CASE("select robust: window and crossing skip locked, frozen and switched-off layers") {
    Big& b = big();
    const auto& idx = b.core->spatialIndex();
    // Cells 100..299 x 50..149: all inside the window.
    const BBox2 box = Big::cellBox(100, 299, 50, 149);
    auto t = std::chrono::steady_clock::now();
    const auto win = selectInBox(b.core->document(), idx, box, false);
    const double winMs = msSince(t);
    t = std::chrono::steady_clock::now();
    const auto cross = selectInBox(b.core->document(), idx, box, true);
    const double crossMs = msSince(t);
    CHECK(win.size() == expectedLines(100, 299, 50, 149));
    CHECK(cross.size() == win.size());
    for (EntityId id : win)
        CHECK(isSelectable(b.core->document(), *b.core->document().find(id)));

    // A box that cuts through the line at its left and right edges: crossing takes the two cut
    // columns, the window does not.
    const BBox2 cut{{100 * kPitch + 10, 50 * kPitch - 5}, {299 * kPitch + 10, 149 * kPitch + 15}};
    const auto w2 = selectInBox(b.core->document(), idx, cut, false);
    const auto c2 = selectInBox(b.core->document(), idx, cut, true);
    CHECK(w2.size() == expectedLines(101, 298, 50, 149));
    CHECK(c2.size() == expectedLines(100, 299, 50, 149));
    std::printf("[select robust] window %.0f ms (%zu), crossing %.0f ms (%zu)\n", winMs, win.size(),
                crossMs, cross.size());
#ifdef NDEBUG
    CHECK(winMs < 100);
    CHECK(crossMs < 100);
#endif
}

TEST_CASE("select robust: block inserts select as one object, never their contents") {
    Big& b = big();
    const auto& idx = b.core->spatialIndex();
    // OUTER spans 50 x 10 at its insertion point (INNER at x 0 and 20, each 10 wide: 0..30).
    const BBox2 box{{-5, kInsertY0 - 5},
                    {kInsertCols * kInsertPitch, kInsertY0 + 10 * kInsertPitch}};
    const auto got = selectInBox(b.core->document(), idx, box, true);
    std::size_t expected = 0;
    for (int j = 0; j < kInserts; ++j)
        if (j / kInsertCols <= 10 && pickableSlot(j))
            ++expected;
    CHECK(got.size() == expected);
    for (EntityId id : got) {
        const Entity* e = b.core->document().find(id);
        REQUIRE(e);
        CHECK(entityAs<InsertData>(*e));
        CHECK(b.core->document().ownerBlock(id) == BlockId{});
    }
    // A window over the middle of one insert's inner block takes nothing: the contents are
    // not selectable and the insert is not wholly inside.
    const BBox2 tiny{{1, kInsertY0 + 1}, {9, kInsertY0 + 9}};
    CHECK(selectInBox(b.core->document(), idx, tiny, false).empty());
    CHECK(selectInBox(b.core->document(), idx, tiny, true).size() == 1);  // insert 0, plain layer
}

TEST_CASE("select robust: WPolygon, CPolygon and fence agree with brute force") {
    Big& b = big();
    const auto& idx = b.core->spatialIndex();
    // A diamond-free axis-aligned polygon (L shape) over cells 0..199 x 0..99, minus the corner
    // block 100..199 x 50..99.
    auto cellPoly = [](int c0, int c1, int r0, int r1) {
        const BBox2 bx = Big::cellBox(c0, c1, r0, r1);
        return std::vector<Vec2>{
            {bx.min.x, bx.min.y}, {bx.max.x, bx.min.y}, {bx.max.x, bx.max.y}, {bx.min.x, bx.max.y}};
    };
    auto t = std::chrono::steady_clock::now();
    const auto wp = selectInPolygon(b.core->document(), idx, cellPoly(0, 199, 0, 99), false);
    const double wpMs = msSince(t);
    t = std::chrono::steady_clock::now();
    const auto cp = selectInPolygon(b.core->document(), idx, cellPoly(0, 199, 0, 99), true);
    const double cpMs = msSince(t);
    CHECK(wp.size() == expectedLines(0, 199, 0, 99));
    CHECK(cp.size() == wp.size());

    std::vector<Vec2> L{{-5, -5},
                        {200 * kPitch - 10, -5},
                        {200 * kPitch - 10, 50 * kPitch - 10},
                        {100 * kPitch - 10, 50 * kPitch - 10},
                        {100 * kPitch - 10, 100 * kPitch - 10},
                        {-5, 100 * kPitch - 10}};
    const auto lw = selectInPolygon(b.core->document(), idx, L, false);
    // Window: the lower 200 x 50 block plus the upper-left 100 x 50 block.
    CHECK(lw.size() == expectedLines(0, 199, 0, 49) + expectedLines(0, 99, 50, 99));

    // A horizontal fence at y = 5 through the middle of row 10 crosses every line in the row
    // (each runs y 300..310 diagonal, so the fence at y 305 touches all of them).
    const std::vector<Vec2> fence{{-100, 10 * kPitch + 5}, {kCols * kPitch + 100, 10 * kPitch + 5}};
    t = std::chrono::steady_clock::now();
    const auto fe = selectAlongFence(b.core->document(), idx, fence);
    const double feMs = msSince(t);
    CHECK(fe.size() == expectedLines(0, kCols - 1, 10, 10));
    std::printf("[select robust] wpolygon %.0f ms, cpolygon %.0f ms, fence %.0f ms\n", wpMs, cpMs,
                feMs);
#ifdef NDEBUG
    CHECK(wpMs < 200);
    CHECK(cpMs < 200);
    CHECK(feMs < 100);
#endif
}

TEST_CASE("select robust: typed Window, Fence, All and Previous at a Select objects prompt") {
    Big& b = big();
    AppCore& core = *b.core;
    auto at = [](double x, double y) {
        char s[64];
        std::snprintf(s, sizeof s, "%.1f,%.1f", x, y);
        return std::string(s);
    };
    core.clearSelection();
    const BBox2 box = Big::cellBox(0, 99, 0, 99);
    core.submitText("SELECT");
    core.submitText("W");
    core.submitText(at(box.min.x, box.min.y));
    core.submitText(at(box.max.x, box.max.y));
    core.submitText("");
    CHECK(core.selection().size() == expectedLines(0, 99, 0, 99));

    // Previous reuses exactly that set.
    const auto first = asSet(core.selection());
    core.clearSelection();
    core.submitText("SELECT");
    core.submitText("P");
    core.submitText("");
    CHECK(asSet(core.selection()) == first);

    // Remove a window of it again.
    core.submitText("SELECT");
    core.submitText("P");
    core.submitText("R");
    const BBox2 rm = Big::cellBox(0, 49, 0, 99);
    core.submitText("W");
    core.submitText(at(rm.min.x, rm.min.y));
    core.submitText(at(rm.max.x, rm.max.y));
    core.submitText("");
    CHECK(core.selection().size() == expectedLines(50, 99, 0, 99));

    // ALL takes only what the user could pick by hand.
    core.clearSelection();
    auto t = std::chrono::steady_clock::now();
    core.submitText("SELECT");
    core.submitText("ALL");
    core.submitText("");
    const double allMs = msSince(t);
    std::size_t expected = 0;
    for (int i = 0; i < kLines; ++i)
        if (pickableSlot(i))
            ++expected;
    for (int j = 0; j < kInserts; ++j)
        if (pickableSlot(j))
            ++expected;
    CHECK(core.selection().size() == expected);
    std::printf("[select robust] SELECT ALL %.0f ms (%zu)\n", allMs, core.selection().size());
    core.clearSelection();
}

TEST_CASE("select robust: QSELECT and FILTER queries over the whole drawing") {
    Big& b = big();
    const Document& doc = b.core->document();
    auto count = [&](std::optional<std::vector<EntityId>> v) {
        REQUIRE(v);
        return v->size();
    };
    auto t = std::chrono::steady_clock::now();
    const auto lines = select::queryEntities(doc, {select::Property::Type, "LINE", false});
    const double typeMs = msSince(t);
    REQUIRE(lines);
    // Frozen and switched-off layers are not visible; locked layers still are.
    std::size_t visibleLines = 0;
    for (int i = 0; i < kLines; ++i)
        if (i % 10 < 8)
            ++visibleLines;
    CHECK(lines->size() == visibleLines);
    // No line of a block definition is ever returned.
    for (EntityId id : *lines)
        CHECK(doc.ownerBlock(id) == BlockId{});

    std::size_t onP3 = 0;
    for (int i = 0; i < kLines; ++i)
        if (i % 10 == 3)
            ++onP3;
    CHECK(count(select::queryEntities(doc, {select::Property::Layer, "P3", false})) ==
          onP3 + kInserts / 10);
    CHECK(count(select::queryEntities(doc, {select::Property::Layer, "LOCK", false})) ==
          kLines / 10 + kInserts / 10);
    CHECK(count(select::queryEntities(doc, {select::Property::Layer, "FROZEN", false})) == 0);
    CHECK(count(select::queryEntities(doc, {select::Property::Layer, "OFF", false})) == 0);
    CHECK(count(select::queryEntities(doc, {select::Property::Type, "INSERT", false})) ==
          kInserts - kInserts / 10 * 2);

    t = std::chrono::steady_clock::now();
    const auto f = select::filterEntities(
        doc, {{select::Property::Type, "LINE", false}, {select::Property::Layer, "P3", false}});
    const double filterMs = msSince(t);
    REQUIRE(f);
    CHECK(f->size() == onP3);
    CHECK(count(select::filterEntities(doc, {{select::Property::Type, "LINE", false},
                                             {select::Property::Layer, "P3", true}})) ==
          visibleLines - onP3);
    CHECK(count(select::filterEntities(doc, {{select::Property::Type, "INSERT", false},
                                             {select::Property::Layer, "P3", false}})) ==
          kInserts / 10);
    std::printf("[select robust] QSELECT type %.0f ms, FILTER two queries %.0f ms\n", typeMs,
                filterMs);
#ifdef NDEBUG
    CHECK(typeMs < 500);
    CHECK(filterMs < 500);
#endif
}
