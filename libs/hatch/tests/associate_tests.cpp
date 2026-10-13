#include <doctest/doctest.h>

#include "helpers.h"
#include "tbcad/core/document.h"
#include "tbcad/hatch/hatch.h"

using namespace tbcad;
using namespace tbcad::hatch;
using namespace tbcad::hatch::testing;

namespace {

struct Fixture {
    Document doc;
    std::vector<EntityId> sides;
    EntityId hatch{};

    Fixture() {
        Transaction tx = doc.begin("setup");
        Vec2 lo{0, 0}, hi{100, 60};
        sides.push_back(tx.add(line(lo, {hi.x, lo.y})).value());
        sides.push_back(tx.add(line({hi.x, lo.y}, hi)).value());
        sides.push_back(tx.add(line(hi, {lo.x, hi.y})).value());
        sides.push_back(tx.add(line({lo.x, hi.y}, lo)).value());
        Scene s;
        s.rectLines(lo, hi);
        // Build the boundary from the document's own entities.
        std::vector<Candidate> c;
        for (EntityId id : sides)
            c.push_back({id, doc.find(id)});
        auto b = findBoundary(c, {50, 30});
        REQUIRE(b.ok());
        const Pattern* p = PatternRegistry::find("SOLID");
        REQUIRE(p);
        hatch = tx.add(makeHatchEntity(b.value(), *p, 1, 0, {0, 0})).value();
        tx.commit();
    }

    const HatchData& data() const { return *entityAs<HatchData>(*doc.find(hatch)); }
    BBox2 box() const { return hatchBounds(data()); }

    // Runs `edit` then the associative update, as the command framework does at commit.
    std::size_t edit(const std::function<void(Transaction&)>& fn) {
        Transaction tx = doc.begin("edit");
        fn(tx);
        std::size_t n = updateAssociativeHatches(doc, tx);
        tx.commit();
        return n;
    }
};

}  // namespace

TEST_CASE("a hatch follows its boundary when a source entity moves") {
    Fixture f;
    CHECK(f.box().max.x == doctest::Approx(100));
    // Stretch the rectangle: move the right side and the ends of top and bottom.
    std::size_t n = f.edit([&](Transaction& tx) {
        (void)tx.modify(f.sides[0], [](Entity& e) { (*entityAs<LineData>(e)).b = {150, 0}; });
        (void)tx.modify(f.sides[1], [](Entity& e) {
            auto& l = *entityAs<LineData>(e);
            l.a = {150, 0};
            l.b = {150, 60};
        });
        (void)tx.modify(f.sides[2], [](Entity& e) { (*entityAs<LineData>(e)).a = {150, 60}; });
    });
    CHECK(n == 1);
    CHECK(f.box().max.x == doctest::Approx(150));
    CHECK(f.data().sourceBoundary.size() == 4);
}

TEST_CASE("erasing a source entity detaches the hatch but keeps its loops") {
    Fixture f;
    f.edit([&](Transaction& tx) { (void)tx.erase(f.sides[1]); });
    REQUIRE(f.doc.find(f.hatch));
    CHECK(f.data().sourceBoundary.empty());
    CHECK(f.box().max.x == doctest::Approx(100));
    CHECK(f.data().loops.size() == 1);
}

TEST_CASE("a source that no longer closes detaches the hatch") {
    Fixture f;
    f.edit([&](Transaction& tx) {
        (void)tx.modify(f.sides[1], [](Entity& e) {
            auto& l = *entityAs<LineData>(e);
            l.a = {300, 200};
            l.b = {300, 260};
        });
    });
    CHECK(f.data().sourceBoundary.empty());
}

TEST_CASE("changes that touch no source leave the hatch alone") {
    Fixture f;
    EntityId other;
    {
        Transaction tx = f.doc.begin("other");
        other = tx.add(line({500, 500}, {600, 600})).value();
        tx.commit();
    }
    std::size_t n = f.edit([&](Transaction& tx) {
        (void)tx.modify(other, [](Entity& e) { (*entityAs<LineData>(e)).b = {700, 700}; });
    });
    CHECK(n == 0);
    CHECK(f.data().sourceBoundary.size() == 4);
}

TEST_CASE("a source change that keeps the geometry does not rewrite the hatch") {
    Fixture f;
    std::size_t n = f.edit([&](Transaction& tx) {
        (void)tx.modify(f.sides[0], [](Entity& e) { e.common.color = Color::byLayer(); });
    });
    CHECK(n == 0);
}
