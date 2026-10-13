#include "test_util.h"

using namespace tbcad;
using namespace tbcad::draw2::test;
using cmd::ScriptItem;

TEST_CASE("POINT places points until Enter") {
    Fixture f;
    CHECK(f.host.run("PO", {"1,2", "3,4", "5,6", ""}).finished);
    auto es = f.entities();
    REQUIRE(es.size() == 3);
    CHECK(close(as<PointData>(es[0]).p, {1, 2}));
    CHECK(close(as<PointData>(es[2]).p, {5, 6}));
    CHECK(f.host.undoStack().undoCount() == 1);
    f.host.undo();
    CHECK(f.entities().empty());
}

TEST_CASE("POINT: Esc keeps the points already placed") {
    Fixture f;
    f.host.run("POINT", {"1,2", "3,4", ScriptItem::esc()});
    CHECK(f.entities().size() == 2);
}

TEST_CASE("PTYPE stores the style and size in the drawing and is undoable") {
    Fixture f;
    CHECK(f.host.run("PTYPE", {"35", "2.5"}).finished);
    CHECK(f.doc.header().custom.at("pdmode") == "35");
    CHECK(std::stod(f.doc.header().custom.at("pdsize")) == 2.5);
    f.host.undo();
    CHECK(f.doc.header().custom.count("pdmode") == 0);
}

TEST_CASE("PTYPE offers the current values as defaults") {
    Fixture f;
    f.host.run("PTYPE", {"3", "4"});
    f.host.run("PTYPE", {"", ""});
    CHECK(f.doc.header().custom.at("pdmode") == "3");
    CHECK(std::stod(f.doc.header().custom.at("pdsize")) == 4);
}

TEST_CASE("PTYPE refuses an unknown style") {
    Fixture f;
    f.host.run("PTYPE", {"5", "1"});
    CHECK(f.host.logContains("must be 0 to 4"));
    CHECK(f.doc.header().custom.count("pdmode") == 0);
}
