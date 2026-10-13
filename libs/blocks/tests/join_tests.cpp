#include "support.h"

using namespace tbcad;
using namespace tbcad::blocks;
using namespace tbcad::blocks::test;

namespace {

Entity rect(double w, double h) {
    Entity e;
    PolylineData p;
    p.closed = true;
    p.v = {{{0, 0}}, {{w, 0}}, {{w, h}}, {{0, h}}};
    e.data = std::move(p);
    return e;
}

struct Walls {
    Document doc;
    BlockId block{};

    explicit Walls(const char* joinClass = "wall") {
        Transaction tx = doc.begin("wall");
        block = tx.addBlock(BlockDef{.name = "Wall"}).value();
        REQUIRE(tx.add(rect(10, 2), block).ok());
        REQUIRE(tx.add(line(0, 1, 10, 1), block).ok());  // stands in for a hatch line
        AssemblyDef as;
        AsmParam len;
        len.name = "L";
        len.defaultNumber = 10;
        as.params.push_back(len);
        as.joinClass = joinClass;
        REQUIRE(tx.modifyBlock(block, [&](BlockDef& d) { d.assembly = std::move(as); }).ok());
        tx.commit();
    }

    EntityId place(Vec2 at, double rot = 0, bool joinOff = false) {
        Entity e = insertOf(block, at, {1, 1}, rot);
        entityAs<InsertData>(e)->joinOff = joinOff;
        return addEntity(doc, std::move(e));
    }

    double drawnLength(EntityId id) {
        registerInsertDisplay();
        geom::DisplayContext ctx;
        ctx.pixelSizeMm = 0.01;
        DisplayList l;
        geom::buildDisplay(doc, *doc.find(id), ctx, l);
        double total = 0;
        for (const DisplayItem& item : l.items)
            if (const auto* pl = std::get_if<DisplayPolyline>(&item)) {
                for (std::size_t i = 0; i + 1 < pl->pts.size(); ++i)
                    total += distance(pl->pts[i], pl->pts[i + 1]);
                if (pl->closed)
                    total += distance(pl->pts.back(), pl->pts.front());
            }
        return total;
    }
};

}  // namespace

TEST_CASE("join: a lone assembly is drawn unchanged") {
    Walls w;
    EntityId a = w.place({0, 0});
    CHECK(w.drawnLength(a) == doctest::Approx(24 + 10));
}

TEST_CASE("join: a T of two walls reads as one outline") {
    Walls w;
    EntityId a = w.place({0, 0});
    EntityId b = w.place({6, 0}, kPi / 2);  // runs up from the first wall's lower edge
    // Outline of the union is 40; each wall loses the hatch line part inside the other (2 each).
    const double total = w.drawnLength(a) + w.drawnLength(b);
    CHECK(total == doctest::Approx(40 + 8 + 8));
}

TEST_CASE("join: an L, an X and walls that only touch") {
    {  // L: the second wall starts at the end of the first and overlaps one corner square
        Walls w;
        EntityId a = w.place({0, 0});
        EntityId b = w.place({10, 0}, kPi / 2);
        // union is an L of two 10x2 bars sharing a 2x2 corner: perimeter 24 + 24 - 8
        CHECK(w.drawnLength(a) + w.drawnLength(b) == doctest::Approx(40 + 8 + 8));
    }
    {  // X: crossing bars share a 2x2 square in the middle; union perimeter is 24+24-8+... = 40
        Walls w;
        EntityId a = w.place({0, 0});
        EntityId b = w.place({6, -4}, kPi / 2);
        CHECK(w.drawnLength(a) + w.drawnLength(b) == doctest::Approx(56));
    }
    {  // apart: nothing changes
        Walls w;
        EntityId a = w.place({0, 0});
        EntityId b = w.place({0, 20});
        CHECK(w.drawnLength(a) + w.drawnLength(b) == doctest::Approx(68));
    }
}

TEST_CASE("join: other classes and un-joined walls keep their own outlines") {
    Walls w;
    EntityId a = w.place({0, 0});
    EntityId b = w.place({6, 0}, kPi / 2, true);  // joinOff
    CHECK(w.drawnLength(a) + w.drawnLength(b) == doctest::Approx(68));
    Walls none("");
    EntityId c = none.place({0, 0});
    EntityId d = none.place({6, 0}, kPi / 2);
    CHECK(none.drawnLength(c) + none.drawnLength(d) == doctest::Approx(68));
}

TEST_CASE("join: moving a member changes the result live") {
    Walls w;
    EntityId a = w.place({0, 0});
    EntityId b = w.place({0, 20});
    CHECK(w.drawnLength(a) + w.drawnLength(b) == doctest::Approx(68));
    {
        Transaction tx = w.doc.begin("move");
        REQUIRE(tx.modify(b, [](Entity& e) {
                      entityAs<InsertData>(e)->position = {6, 0};
                      entityAs<InsertData>(e)->rotation = kPi / 2;
                  }).ok());
        tx.commit();
    }
    CHECK(w.drawnLength(a) + w.drawnLength(b) == doctest::Approx(56));
}
