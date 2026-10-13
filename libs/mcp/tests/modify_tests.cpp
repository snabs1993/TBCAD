#include <cmath>

#include "rig.h"

using namespace tbcad;
using namespace tbcad::mcp;
using namespace tbcad::mcp::test;

namespace {

// Adds entities and returns their ids as decimal strings.
std::vector<std::string> add(Rig& r, const std::string& entitiesJson) {
    Json out = r.ok("add_entities", J("{\"entities\":" + entitiesJson + "}"));
    std::vector<std::string> ids;
    for (const Json& j : out.find("ids")->asArray())
        ids.push_back(j.asString());
    return ids;
}

Json entity(Rig& r, const std::string& id) {
    return r.ok("get_entity", J("{\"id\":\"" + id + "\"}"));
}

Json arg(const std::string& text) {
    return J(text);
}

}  // namespace

TEST_CASE("offset_entities: left and right of a line, one undo step") {
    Rig r;
    r.initialize();
    auto ids = add(r, R"([{"kind":"line","start":[0,0],"end":[100,0]}])");
    std::string before = r.dumpDoc();
    Json res = r.ok("offset_entities",
                    arg("{\"ids\":[\"" + ids[0] + "\"],\"distance\":10,\"side\":\"left\"}"));
    REQUIRE(res.find("created")->asArray().size() == 1);
    Json e = entity(r, res.find("created")->asArray()[0].asString());
    CHECK(e.find("start")->asArray()[1].asNumber() == doctest::Approx(10));
    res = r.ok("offset_entities",
               arg("{\"ids\":[\"" + ids[0] + "\"],\"distance\":10,\"side_point\":[50,-3]}"));
    e = entity(r, res.find("created")->asArray()[0].asString());
    CHECK(e.find("start")->asArray()[1].asNumber() == doctest::Approx(-10));
    CHECK(r.session.undoStack().undoCount() == 3);  // add + two offsets
    CHECK(r.session.undo());
    CHECK(r.session.undo());
    CHECK(r.dumpDoc() == before);
}

TEST_CASE("offset_entities: argument errors are explained") {
    Rig r;
    r.initialize();
    auto ids = add(r, R"([{"kind":"line","start":[0,0],"end":[100,0]}])");
    CHECK(r.fails("offset_entities", arg("{\"ids\":[\"" + ids[0] + "\"],\"distance\":10}"))
              .find("side") != std::string::npos);
    CHECK(r.fails("offset_entities",
                  arg("{\"ids\":[\"" + ids[0] + "\"],\"distance\":0,\"side\":\"left\"}"))
              .find("distance") != std::string::npos);
    CHECK(r.fails("offset_entities", arg("{\"ids\":[\"999\"],\"distance\":5,\"side\":\"left\"}"))
              .find("999") != std::string::npos);
}

TEST_CASE("trim_entities and extend_entities cut and grow to the edges") {
    Rig r;
    r.initialize();
    auto ids = add(r, R"([{"kind":"line","start":[0,0],"end":[100,0]},
                          {"kind":"line","start":[40,-50],"end":[40,50]},
                          {"kind":"line","start":[70,-50],"end":[70,50]}])");
    // trim the middle of the long line between the two cutting lines
    Json res = r.ok("trim_entities",
                    arg("{\"targets\":[{\"id\":\"" + ids[0] + "\",\"pick\":[55,0]}],\"edges\":[\"" +
                        ids[1] + "\",\"" + ids[2] + "\"]}"));
    CHECK(res.find("created")->asArray().size() + res.find("modified")->asArray().size() +
              res.find("erased")->asArray().size() >=
          2);
    // a short line is extended to the boundary
    auto more = add(r, R"([{"kind":"line","start":[0,20],"end":[30,20]}])");
    res =
        r.ok("extend_entities", arg("{\"targets\":[{\"id\":\"" + more[0] +
                                    "\",\"pick\":[30,20]}],\"boundaries\":[\"" + ids[1] + "\"]}"));
    Json e = entity(r, more[0]);
    CHECK(e.find("end")->asArray()[0].asNumber() == doctest::Approx(40));
    // an edge that never crosses the target: the call fails and changes nothing
    auto far = add(r, R"([{"kind":"line","start":[500,-50],"end":[500,50]}])");
    std::string before = r.dumpDoc();
    CHECK_FALSE(
        r.fails("trim_entities", arg("{\"targets\":[{\"id\":\"" + more[0] +
                                     "\",\"pick\":[10,20]}],\"edges\":[\"" + far[0] + "\"]}"))
            .empty());
    CHECK(r.dumpDoc() == before);
}

TEST_CASE("fillet and chamfer on two lines and on a polyline") {
    Rig r;
    r.initialize();
    auto ids = add(r, R"([{"kind":"line","start":[0,0],"end":[100,0]},
                          {"kind":"line","start":[100,-100],"end":[100,100]}])");
    Json res = r.ok("fillet", arg("{\"first\":{\"id\":\"" + ids[0] +
                                  "\",\"pick\":[80,0]},"
                                  "\"second\":{\"id\":\"" +
                                  ids[1] + "\",\"pick\":[100,50]},\"radius\":20}"));
    REQUIRE(res.find("created")->asArray().size() == 1);
    Json arc = entity(r, res.find("created")->asArray()[0].asString());
    CHECK(arc.find("kind")->asString() == "arc");
    CHECK(arc.find("radius")->asNumber() == doctest::Approx(20));

    auto lines = add(r, R"([{"kind":"line","start":[0,200],"end":[100,200]},
                            {"kind":"line","start":[100,100],"end":[100,300]}])");
    res = r.ok("chamfer",
               arg("{\"first\":{\"id\":\"" + lines[0] +
                   "\",\"pick\":[80,200]},"
                   "\"second\":{\"id\":\"" +
                   lines[1] + "\",\"pick\":[100,250]},\"distance1\":10,\"distance2\":10}"));
    REQUIRE(res.find("created")->asArray().size() == 1);
    CHECK(entity(r, res.find("created")->asArray()[0].asString()).find("kind")->asString() ==
          "line");

    auto pl = add(r, R"([{"kind":"polyline","vertices":[[0,400],[100,400],[100,500]]}])");
    res = r.ok("fillet", arg("{\"polyline\":\"" + pl[0] + "\",\"radius\":15}"));
    CHECK(res.find("modified")->asArray().size() == 1);
    CHECK(r.fails("fillet", arg("{\"first\":{\"id\":\"" + ids[0] + "\",\"pick\":[1,0]}}"))
              .find("second") != std::string::npos);
}

TEST_CASE("break_entity splits or removes a part; join_entities puts pieces back") {
    Rig r;
    r.initialize();
    auto ids = add(r, R"([{"kind":"line","start":[0,0],"end":[100,0]}])");
    Json res = r.ok("break_entity",
                    arg("{\"id\":\"" + ids[0] + "\",\"point1\":[30,0],\"point2\":[60,0]}"));
    CHECK(res.find("modified")->asArray().size() + res.find("created")->asArray().size() == 2);
    CHECK(r.session.document().modelSpaceCount() == 2);

    Rig j;
    j.initialize();
    auto parts = add(j, R"([{"kind":"line","start":[0,0],"end":[50,0]},
                            {"kind":"line","start":[50,0],"end":[100,0]}])");
    Json joined =
        j.ok("join_entities", arg("{\"ids\":[\"" + parts[0] + "\",\"" + parts[1] + "\"]}"));
    CHECK(joined.find("created")->asArray().size() == 1);
    CHECK(joined.find("erased")->asArray().size() == 2);
    CHECK(j.session.document().modelSpaceCount() == 1);
    CHECK(j.fails("join_entities", arg("{\"ids\":[\"" + parts[0] + "\"]}")).find("two") !=
          std::string::npos);
}

TEST_CASE("modify tools refuse locked layers and non-curve entities") {
    Rig r;
    r.initialize();
    r.ok("create_layer", J(R"({"name":"WALLS"})"));
    auto ids = add(r, R"([{"kind":"line","start":[0,0],"end":[100,0],"layer":"WALLS"},
                          {"kind":"text","position":[0,0],"height":5,"value":"hi"}])");
    r.ok("modify_layer", J(R"({"path":"WALLS","locked":true})"));
    std::string before = r.dumpDoc();
    CHECK(r.fails("offset_entities",
                  arg("{\"ids\":[\"" + ids[0] + "\"],\"distance\":5,\"side\":\"left\"}"))
              .find("locked") != std::string::npos);
    CHECK(r.fails("offset_entities",
                  arg("{\"ids\":[\"" + ids[1] + "\"],\"distance\":5,\"side\":\"left\"}"))
              .find("text") != std::string::npos);
    CHECK(r.dumpDoc() == before);
}
