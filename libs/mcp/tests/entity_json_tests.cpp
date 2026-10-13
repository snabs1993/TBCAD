#include "rig.h"
#include "tbcad/core/math.h"

using namespace tbcad;
using namespace tbcad::mcp;
using namespace tbcad::mcp::test;

namespace {

std::string addOne(Rig& r, const std::string& entity) {
    Json out = r.ok("add_entities", J("{\"entities\":[" + entity + "]}"));
    return out.find("ids")->asArray()[0].asString();
}

Json get(Rig& r, const std::string& id) {
    return r.ok("get_entity", J("{\"id\":\"" + id + "\"}"));
}

void makeBlock(Rig& r, const std::string& name) {
    Document& doc = r.session.document();
    auto tx = doc.begin("block");
    BlockDef b;
    b.name = name;
    REQUIRE(tx.addBlock(b));
    (void)tx.commit();
}

}  // namespace

TEST_CASE("entity json: hatch gradients and fill colours round trip") {
    Rig r;
    r.initialize();
    std::string id = addOne(r, R"({"kind":"hatch","loops":[[[0,0],[100,0],[100,100],[0,100]]],
        "gradient":{"type":"cylinder","color1":"#102030","color2":3,"angle":{"deg":45}}})");
    Json h = get(r, id);
    REQUIRE(h.find("gradient"));
    CHECK(h.find("gradient")->find("type")->asString() == "cylinder");
    CHECK(h.find("gradient")->find("angle")->asNumber() == doctest::Approx(tbcad::kPi / 4));
    CHECK(h.find("gradient")->find("color1")->asString() == "#102030");
    // the gradient survives a modify that changes something else
    r.ok("modify_entities", J("{\"ids\":[\"" + id + "\"],\"set\":{\"color\":1}}"));
    CHECK(get(r, id).find("gradient")->find("type")->asString() == "cylinder");
    CHECK(r.fails("add_entities", J(R"({"entities":[{"kind":"hatch","loops":[[[0,0],[1,0],[1,1]]],
                      "gradient":{"type":"swirl"}}]})"))
              .find("gradient type") != std::string::npos);
    // an ordinary pattern hatch keeps pattern, scale and origin
    id = addOne(r, R"({"kind":"hatch","loops":[[[0,0],[100,0],[100,100]]],"pattern":"ANSI31",
        "scale":2,"patternOrigin":[5,5],"fill":1})");
    Json p = get(r, id);
    CHECK(p.find("pattern")->asString() == "ANSI31");
    CHECK(p.find("patternOrigin")->asArray()[0].asNumber() == 5);
    CHECK(p.find("fill")->asNumber() == 1);
}

TEST_CASE("entity json: insert attributes, dynamic values and arrays") {
    Rig r;
    r.initialize();
    makeBlock(r, "DOOR");
    std::string id = addOne(r, R"({"kind":"insert","block":"DOOR","position":[10,20],
        "attributes":{"TAG":"D-01","SIZE":"900"},"dynamicValues":[1200],
        "array":{"kind":"rectangular","rows":2,"columns":3,"rowSpacing":500,"columnSpacing":1000}})");
    Json in = get(r, id);
    CHECK(in.find("attributes")->find("TAG")->asString() == "D-01");
    CHECK(in.find("dynamicValues")->asArray()[0].asNumber() == 1200);
    CHECK(in.find("array")->find("kind")->asString() == "rectangular");
    CHECK(in.find("array")->find("columns")->asNumber() == 3);

    std::string polar = addOne(r, R"({"kind":"insert","block":"DOOR","position":[0,0],
        "array":{"kind":"polar","center":[100,0],"count":6,"fillAngle":{"deg":180}}})");
    CHECK(get(r, polar).find("array")->find("count")->asNumber() == 6);
    CHECK(
        r.fails("add_entities", J(R"({"entities":[{"kind":"insert","block":"DOOR","position":[0,0],
                      "array":{"kind":"spiral"}}]})"))
            .find("array kind") != std::string::npos);
    CHECK(
        r.fails("add_entities", J(R"({"entities":[{"kind":"insert","block":"DOOR","position":[0,0],
                      "array":{"kind":"rectangular","rows":0}}]})"))
            .find("rows") != std::string::npos);
}

TEST_CASE("entity json: dimensions of every kind and leaders can be written and read back") {
    Rig r;
    r.initialize();
    std::string lin = addOne(
        r,
        R"({"kind":"dimension","dimKind":"linear","start":[0,0],"end":[6000,0],"dimLine":[3000,-600]})");
    Json d = get(r, lin);
    CHECK(d.find("dimKind")->asString() == "linear");
    CHECK(d.find("measurement")->asNumber() == doctest::Approx(6000));
    CHECK(d.find("textPositionFixed")->asBool() == false);

    std::string vertical =
        addOne(r, R"({"kind":"dimension","dimKind":"linear","start":[0,0],"end":[3000,4000],
              "dimLine":[-500,0],"rotation":{"deg":90}})");
    CHECK(get(r, vertical).find("measurement")->asNumber() == doctest::Approx(4000));

    std::string aligned = addOne(
        r,
        R"({"kind":"dimension","dimKind":"aligned","start":[0,0],"end":[3000,4000],"dimLine":[3000,0]})");
    CHECK(get(r, aligned).find("measurement")->asNumber() == doctest::Approx(5000));

    std::string rad = addOne(
        r,
        R"({"kind":"dimension","dimKind":"radius","center":[0,0],"radius":250,"angle":{"deg":30}})");
    CHECK(get(r, rad).find("measurement")->asNumber() == doctest::Approx(250));
    std::string dia =
        addOne(r, R"({"kind":"dimension","dimKind":"diameter","center":[0,0],"radius":250})");
    CHECK(get(r, dia).find("measurement")->asNumber() == doctest::Approx(500));

    std::string ang = addOne(
        r, R"({"kind":"dimension","dimKind":"angular","lines":[[[0,0],[100,0]],[[0,0],[0,100]]],
              "arcPoint":[50,50]})");
    CHECK(get(r, ang).find("measurement")->asNumber() == doctest::Approx(tbcad::kPi / 2));
    std::string ang3 =
        addOne(r, R"({"kind":"dimension","dimKind":"angular3","first":[100,0],"second":[0,100],
              "vertex":[0,0],"arcPoint":[50,50]})");
    CHECK(get(r, ang3).find("measurement")->asNumber() == doctest::Approx(tbcad::kPi / 2));

    // text override and a fixed text position
    std::string txt = addOne(
        r,
        R"({"kind":"dimension","dimKind":"linear","start":[0,0],"end":[1000,0],"dimLine":[500,-200],
              "text":"approx <>","textPosition":[700,-400]})");
    Json t = get(r, txt);
    CHECK(t.find("text")->asString() == "approx <>");
    CHECK(t.find("textPositionFixed")->asBool());
    CHECK(t.find("textPosition")->asArray()[0].asNumber() == doctest::Approx(700));

    // the geometry of a dimension is not changed with `set`
    CHECK(r.fails("modify_entities", J("{\"ids\":[\"" + lin + "\"],\"set\":{\"end\":[7000,0]}}"))
              .find("transform") != std::string::npos);

    // leaders
    std::string lead =
        addOne(r, R"({"kind":"leader","vertices":[[0,0],[300,200]],"text":"see note 4"})");
    Json l = get(r, lead);
    CHECK(l.find("text")->asString().find("see note 4") != std::string::npos);
    CHECK(l.find("vertices")->asArray().size() == 2);

    // errors say what is wrong
    CHECK(r.fails("add_entities",
                  J(R"({"entities":[{"kind":"dimension","dimKind":"linear","start":[0,0],
                      "end":[0,0],"dimLine":[0,10]}]})"))
              .find("differ") != std::string::npos);
    CHECK(r.fails("add_entities", J(R"({"entities":[{"kind":"dimension","dimKind":"radius",
                      "center":[0,0],"radius":0}]})"))
              .find("radius") != std::string::npos);
    CHECK(r.fails("add_entities",
                  J(R"({"entities":[{"kind":"dimension","dimKind":"linear","start":[0,0],
                      "end":[10,0],"dimLine":[5,5],"dimStyle":"NoSuchStyle"}]})"))
              .find("NoSuchStyle") != std::string::npos);
    CHECK(r.fails("add_entities", J(R"({"entities":[{"kind":"leader","vertices":[[0,0]]}]})"))
              .find("two vertices") != std::string::npos);
}
