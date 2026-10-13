#include "rig.h"

using namespace tbcad;
using namespace tbcad::mcp;
using namespace tbcad::mcp::test;

namespace {

Json fourWalls() {
    return J(R"({"entities":[
        {"kind":"line","start":[0,0],"end":[4000,0]},
        {"kind":"line","start":[4000,0],"end":[4000,3000]},
        {"kind":"line","start":[4000,3000],"end":[0,3000]},
        {"kind":"line","start":[0,3000],"end":[0,0]}],
      "layer":"A-WALL","create_layers":true})");
}

}  // namespace

TEST_CASE("add_entities then query by layer") {
    Rig r;
    r.initialize();
    Json added = r.ok("add_entities", fourWalls());
    CHECK(added.find("count")->asNumber() == 4);
    CHECK(added.find("createdLayers")->asArray().size() == 1);
    Json q = r.ok("query_entities", J(R"({"layers":["A-WALL"]})"));
    CHECK(q.find("total")->asNumber() == 4);
    CHECK(q.find("entities")->asArray()[0].find("kind")->asString() == "line");
    CHECK(q.find("entities")->asArray()[0].find("layer")->asString() == "A-WALL");
    CHECK(r.session.document().modelSpaceCount() == 4);
    Json info = r.ok("get_drawing_info");
    CHECK(info.find("entityCount")->asNumber() == 4);
    CHECK(info.find("extents")->find("max")->asArray()[0].asNumber() == 4000);
    CHECK(info.find("units")->asString() == "mm");
}

TEST_CASE("add_entities: unknown layer needs create_layers, errors name the entity") {
    Rig r;
    r.initialize();
    std::string before = r.dumpDoc();
    std::string e =
        r.fails("add_entities",
                J(R"({"entities":[{"kind":"line","start":[0,0],"end":[1,1]}],"layer":"X"})"));
    CHECK(e.find("create_layers") != std::string::npos);
    e = r.fails(
        "add_entities",
        J(R"({"entities":[{"kind":"line","start":[0,0],"end":[1,1]},{"kind":"circle","center":[0,0]}]})"));
    CHECK(e.find("entities[1]") != std::string::npos);
    CHECK(e.find("radius") != std::string::npos);
    CHECK(r.dumpDoc() == before);  // the good first entity was rolled back too
    CHECK(r.session.undoStack().undoCount() == 0);
}

TEST_CASE("one AI call is one undo step; undo and redo restore the drawing") {
    Rig r;
    r.initialize();
    std::string before = r.dumpDoc();
    r.ok("add_entities", fourWalls());  // 4 entities and a new layer in one call
    std::string after = r.dumpDoc();
    CHECK(after != before);
    CHECK(r.session.undoStack().undoCount() == 1);
    CHECK(r.session.undoStack().undoLabel() == "AI: add_entities");

    Json u = r.ok("undo");
    CHECK(u.find("undone")->asNumber() == 1);
    CHECK(r.dumpDoc() == before);
    r.ok("redo");
    CHECK(r.dumpDoc() == after);
}

TEST_CASE("undo leaves the user's own steps alone unless forced") {
    Rig r;
    r.initialize();
    // A step the user made.
    {
        auto tx = r.session.document().begin("user line");
        (void)tx.add(Entity{{}, LineData{{0, 0}, {1, 1}}});
        r.session.undoStack().push("Line", {tx.commit()});
    }
    r.ok("add_entities", J(R"({"entities":[{"kind":"point","position":[5,5]}]})"));
    r.ok("undo");
    std::string e = r.fails("undo");
    CHECK(e.find("Line") != std::string::npos);
    CHECK(r.session.document().modelSpaceCount() == 1);
    r.ok("undo", J(R"({"force":true})"));
    CHECK(r.session.document().modelSpaceCount() == 0);
}

TEST_CASE("modify_entities: properties, geometry, transforms") {
    Rig r;
    r.initialize();
    Json added = r.ok("add_entities", J(R"({"entities":[
        {"kind":"circle","center":[0,0],"radius":10},
        {"kind":"line","start":[0,0],"end":[100,0]}]})"));
    std::string c = added.find("ids")->asArray()[0].asString();
    std::string l = added.find("ids")->asArray()[1].asString();

    r.ok("modify_entities", J(R"({"ids":[")" + c + R"("],"set":{"radius":25,"color":1}})"));
    Json e = r.ok("get_entity", J(R"({"id":")" + c + R"("})"));
    CHECK(e.find("radius")->asNumber() == 25);
    CHECK(e.find("color")->asNumber() == 1);

    r.ok("modify_entities", J(R"({"ids":[")" + l + R"("],"transform":{"move":[10,20]}})"));
    e = r.ok("get_entity", J(R"({"id":")" + l + R"("})"));
    CHECK(e.find("start")->asArray()[0].asNumber() == 10);
    CHECK(e.find("end")->asArray()[1].asNumber() == 20);

    r.ok("modify_entities", J(R"({"ids":[")" + l +
                              R"("],"transform":{"rotate":{"base":[10,20],"angle":{"deg":90}}}})"));
    e = r.ok("get_entity", J(R"({"id":")" + l + R"("})"));
    CHECK(e.find("end")->asArray()[0].asNumber() == doctest::Approx(10));
    CHECK(e.find("end")->asArray()[1].asNumber() == doctest::Approx(120));

    std::string err =
        r.fails("modify_entities", J(R"({"ids":[")" + l + R"("],"set":{"radius":3}})"));
    CHECK(err.find("unknown property") != std::string::npos);
    err = r.fails("modify_entities", J(R"({"ids":["999999"],"set":{"color":1}})"));
    CHECK(err.find("999999") != std::string::npos);
}

TEST_CASE("delete_entities and set_selection") {
    Rig r;
    r.initialize();
    Json added = r.ok(
        "add_entities",
        J(R"({"entities":[{"kind":"point","position":[0,0]},{"kind":"point","position":[1,1]}]})"));
    auto ids = added.find("ids")->asArray();
    r.ok("set_selection", J(R"({"ids":[")" + ids[0].asString() + R"("]})"));
    CHECK(r.ok("get_selection").find("count")->asNumber() == 1);
    r.ok("delete_entities", J(R"({"ids":[")" + ids[0].asString() + R"("]})"));
    CHECK(r.session.document().modelSpaceCount() == 1);
    CHECK(r.ok("get_selection").find("count")->asNumber() == 0);
    r.ok("undo");
    CHECK(r.session.document().modelSpaceCount() == 2);
}

TEST_CASE("query_entities: pagination, bbox, kinds") {
    Rig r;
    r.initialize();
    Json::Array ents;
    for (int i = 0; i < 25; ++i) {
        ents.push_back(J("{\"kind\":\"line\",\"start\":[" + std::to_string(i * 10) +
                         ",0],\"end\":[" + std::to_string(i * 10 + 5) + ",5]}"));
    }
    ents.push_back(J(R"({"kind":"circle","center":[500,500],"radius":5})"));
    r.ok("add_entities", Json(Json::Object{{"entities", Json(std::move(ents))}}));

    Json p1 = r.ok("query_entities", J(R"({"limit":10})"));
    CHECK(p1.find("entities")->asArray().size() == 10);
    CHECK(p1.find("total")->asNumber() == 26);
    std::string cursor = p1.find("next_cursor")->asString();
    Json p2 = r.ok("query_entities", J(R"({"limit":10,"cursor":")" + cursor + R"("})"));
    CHECK(p2.find("entities")->asArray()[0].find("id")->asString() ==
          std::to_string(std::stoull(cursor) + 1));
    Json p3 = r.ok("query_entities",
                   J(R"({"limit":10,"cursor":")" + p2.find("next_cursor")->asString() + R"("})"));
    CHECK(p3.find("entities")->asArray().size() == 6);
    CHECK_FALSE(p3.find("next_cursor"));

    Json k = r.ok("query_entities", J(R"({"kinds":["circle"]})"));
    CHECK(k.find("total")->asNumber() == 1);
    Json b = r.ok("query_entities", J(R"({"bbox":[[0,0],[42,10]],"mode":"inside"})"));
    CHECK(b.find("total")->asNumber() == 4);
    Json bc = r.ok("query_entities", J(R"({"bbox":[[0,0],[42,10]]})"));
    CHECK(bc.find("total")->asNumber() == 5);
}

TEST_CASE("units: degrees and other length units are converted") {
    CHECK(angleFromJson(J(R"({"deg":90})")).value() == doctest::Approx(kPi / 2));
    CHECK(lengthFromJson(J(R"({"value":3,"unit":"m"})")).value() == 3000);
    CHECK(lengthFromJson(J(R"({"value":1,"unit":"in"})")).value() == doctest::Approx(25.4));
    CHECK_FALSE(lengthFromJson(J(R"({"value":1,"unit":"parsec"})")).ok());
    CHECK(pointFromJson(J(R"([{"value":1,"unit":"m"},5])")).value() == Vec2{1000, 5});

    Rig r;
    r.initialize();
    Json a = r.ok("add_entities", J(R"({"entities":[
        {"kind":"arc","center":[0,0],"radius":{"value":1,"unit":"m"},"startAngle":0,"endAngle":{"deg":90}}]})"));
    Json e = r.ok("get_entity", J(R"({"id":")" + a.find("ids")->asArray()[0].asString() + R"("})"));
    CHECK(e.find("radius")->asNumber() == 1000);
    CHECK(e.find("endAngle")->asNumber() == doctest::Approx(kPi / 2));
}

TEST_CASE("all writable kinds round trip through JSON") {
    Rig r;
    r.initialize();
    Json added = r.ok("add_entities", J(R"({"entities":[
        {"kind":"line","start":[0,0],"end":[10,10],"color":"#FF8000","lineweight":0.5},
        {"kind":"circle","center":[5,5],"radius":3},
        {"kind":"polyline","vertices":[{"x":0,"y":0,"bulge":0.5},[10,0],[10,10]],"closed":true},
        {"kind":"point","position":[1,2]},
        {"kind":"ellipse","center":[0,0],"majorAxis":[10,0],"ratio":0.5},
        {"kind":"spline","degree":3,"controlPoints":[[0,0],[1,2],[2,2],[3,0]]},
        {"kind":"text","position":[0,0],"height":5,"value":"Hello สวัสดี","rotation":{"deg":30}},
        {"kind":"mtext","position":[0,0],"height":5,"width":100,"value":"A\\PB","attachment":"middleCenter"},
        {"kind":"hatch","loops":[[[0,0],[10,0],[10,10],[0,10]]],"pattern":"ANSI31","scale":2,"angle":{"deg":45}}]})"));
    auto ids = added.find("ids")->asArray();
    REQUIRE(ids.size() == 9);
    const char* kinds[] = {"line",   "circle", "polyline", "point", "ellipse",
                           "spline", "text",   "mtext",    "hatch"};
    for (std::size_t i = 0; i < ids.size(); ++i) {
        Json e = r.ok("get_entity", J(R"({"id":")" + ids[i].asString() + R"("})"));
        CHECK(e.find("kind")->asString() == kinds[i]);
        // Reading and writing back gives the same JSON for the writable kinds.
        if (i != 8) {
            Json copy = e;
            copy.object().erase("id");
            Json again =
                r.ok("add_entities", Json(Json::Object{{"entities", Json(Json::Array{copy})}}));
            Json e2 = r.ok("get_entity",
                           J(R"({"id":")" + again.find("ids")->asArray()[0].asString() + R"("})"));
            e2.object().erase("id");
            CHECK(dump(e2) == dump(copy));
        }
    }
    CHECK(r.ok("get_entity", J(R"({"id":")" + ids[0].asString() + R"("})"))
              .find("color")
              ->asString() == "#FF8000");
    Json t = r.ok("get_entity", J(R"({"id":")" + ids[6].asString() + R"("})"));
    CHECK(t.find("value")->asString() ==
          "Hello \xe0\xb8\xaa\xe0\xb8\xa7\xe0\xb8\xb1\xe0\xb8\xaa\xe0\xb8\x94\xe0\xb8\xb5");
    CHECK(t.find("rotation")->asNumber() == doctest::Approx(kPi / 6));
    Json h = r.ok("get_entity", J(R"({"id":")" + ids[8].asString() + R"("})"));
    CHECK(h.find("pattern")->asString() == "ANSI31");
    CHECK(h.find("loops")->asArray().size() == 1);
}

TEST_CASE("dimensions cannot be written directly") {
    Rig r;
    r.initialize();
    std::string e = r.fails("add_entities", J(R"({"entities":[{"kind":"dimension"}]})"));
    CHECK(e.find("run_command") != std::string::npos);
}

TEST_CASE("sub-layers: create, nest, refuse cycles, delete") {
    Rig r;
    r.initialize();
    r.ok("create_layer", J(R"({"name":"S-FLOOR","color":3})"));
    Json child = r.ok("create_layer", J(R"({"name":"edge","parent":"S-FLOOR"})"));
    CHECK(child.find("path")->asString() == "S-FLOOR~edge");
    CHECK(child.find("color")->asString() == "ByParent");
    r.ok("create_layer", J(R"({"name":"inner","parent":"S-FLOOR~edge"})"));

    Json tree = r.ok("list_layers");
    const auto& layers = tree.find("layers")->asArray();
    const Json* floor = nullptr;
    for (const auto& l : layers) {
        if (l.find("name")->asString() == "S-FLOOR")
            floor = &l;
    }
    REQUIRE(floor);
    REQUIRE(floor->find("children")->asArray().size() == 1);
    const Json& edge = floor->find("children")->asArray()[0];
    CHECK(edge.find("path")->asString() == "S-FLOOR~edge");
    CHECK(edge.find("effective")->find("color")->asNumber() == 3);
    CHECK(edge.find("children")->asArray().size() == 1);

    std::string e =
        r.fails("modify_layer", J(R"({"path":"S-FLOOR","new_parent":"S-FLOOR~edge~inner"})"));
    CHECK(e.find("cycle") != std::string::npos);
    e = r.fails("create_layer", J(R"({"name":"a~b"})"));
    CHECK(e.find("~") != std::string::npos);

    r.ok("modify_layer", J(R"({"path":"S-FLOOR~edge","on":false,"rename":"rim"})"));
    CHECK(r.session.document().layerByName(r.session.document().layerByName("S-FLOOR")->id, "rim"));
    // Turning the parent off hides the branch but keeps each switch.
    r.ok("modify_layer", J(R"({"path":"S-FLOOR","on":false})"));
    Json one = r.ok("list_layers");
    (void)one;
    CHECK_FALSE(
        r.session.document()
            .effective(r.session.document()
                           .layerByName(r.session.document().layerByName("S-FLOOR")->id, "rim")
                           ->id)
            .on);

    // A layer with entities cannot be deleted.
    r.ok("add_entities",
         J(R"({"entities":[{"kind":"point","position":[0,0]}],"layer":"S-FLOOR~rim"})"));
    e = r.fails("delete_layer", J(R"({"path":"S-FLOOR"})"));
    CHECK(e.find("holds 1 entities") != std::string::npos);
    r.ok("delete_layer", J(R"({"path":"S-FLOOR~rim~inner"})"));
    r.ok("set_current_layer", J(R"({"path":"S-FLOOR"})"));
    e = r.fails("delete_layer", J(R"({"path":"S-FLOOR"})"));
    CHECK(e.find("current layer") != std::string::npos);
}

TEST_CASE("delete_layer move_children_up keeps the sub-layers") {
    Rig r;
    r.initialize();
    r.ok("create_layer", J(R"({"name":"P","color":5})"));
    r.ok("create_layer", J(R"({"name":"C","parent":"P"})"));
    r.ok("delete_layer", J(R"({"path":"P","mode":"move_children_up"})"));
    const Layer* c = r.session.document().layerByName("C");
    REQUIRE(c);
    CHECK_FALSE(c->parent);
    CHECK(c->color == Color::index(5));  // resolved from the deleted parent
}

TEST_CASE("run_command: scripted input, incomplete input is rolled back") {
    Rig r;
    r.initialize();
    Json ok = r.ok("run_command", J(R"({"command":"TL","inputs":["0,0","@100,0"]})"));
    CHECK(ok.find("finished")->asBool());
    CHECK(ok.find("createdCount")->asNumber() == 1);
    CHECK(ok.find("messages")->asArray()[0].asString() == "line made");
    CHECK(r.session.undoStack().undoLabel() == "AI: run_command");

    std::string before = r.dumpDoc();
    std::string e = r.fails("run_command", J(R"({"command":"TESTLINE","inputs":["0,0"]})"));
    CHECK(e.find("Second point") != std::string::npos);
    CHECK(r.dumpDoc() == before);
    CHECK_FALSE(r.session.runner().active());

    e = r.fails("run_command", J(R"({"command":"TESTLINE","inputs":["0,0","garbage"]})"));
    CHECK(e.find("garbage") != std::string::npos);
    CHECK(r.dumpDoc() == before);

    e = r.fails("run_command", J(R"({"command":"NOSUCH"})"));
    CHECK(e.find("unknown command") != std::string::npos);
    e = r.fails("run_command", J(R"({"command":"DEVGEN"})"));
    CHECK(e.find("cannot be run") != std::string::npos);

    Json cmds = r.ok("list_commands", J(R"({"filter":"line"})"));
    CHECK(cmds.find("commands")->asArray().size() == 1);
    Json all = r.ok("list_commands");
    for (const auto& c : all.find("commands")->asArray())
        CHECK(c.find("name")->asString() != "DEVGEN");
}

TEST_CASE("busy: a running user command blocks edits but not reads") {
    Rig r;
    r.initialize();
    REQUIRE(r.session.runner().start("TESTLINE").ok());
    std::string before = r.dumpDoc();
    std::string e =
        r.fails("add_entities", J(R"({"entities":[{"kind":"point","position":[0,0]}]})"));
    CHECK(e.find("TESTLINE") != std::string::npos);
    CHECK(e.find("try again") != std::string::npos);
    CHECK(r.dumpDoc() == before);
    Json info = r.ok("get_drawing_info");
    CHECK(info.find("commandRunning")->asBool());
    r.ok("list_layers");
    (void)r.session.runner().submit(cmd::Input::cancel());
}

TEST_CASE("permission: deny, read-only and allow-once") {
    {
        Rig r;
        r.session.setPolicy([](const ClientInfo&) { return EditDecision::Deny; });
        r.initialize();
        std::string before = r.dumpDoc();
        std::string e =
            r.fails("add_entities", J(R"({"entities":[{"kind":"point","position":[0,0]}]})"));
        CHECK(e.find("not allowed") != std::string::npos);
        CHECK(r.dumpDoc() == before);
        r.ok("get_drawing_info");
    }
    {
        Rig r;
        r.session.setReadOnly(true);
        r.initialize();
        r.fails("create_layer", J(R"({"name":"X"})"));
        r.fails("undo");
        r.ok("query_entities");
    }
    {
        Rig r;
        int asked = 0;
        std::string who;
        r.session.setPolicy([&](const ClientInfo& c) {
            ++asked;
            who = c.name;
            return EditDecision::Allow;
        });
        r.initialize();
        r.ok("create_layer", J(R"({"name":"A"})"));
        r.ok("create_layer", J(R"({"name":"B"})"));
        r.ok("add_entities", J(R"({"entities":[{"kind":"point","position":[0,0]}]})"));
        CHECK(asked == 1);
        CHECK(who == "test-client");
    }
}

TEST_CASE("measure") {
    Rig r;
    r.initialize();
    CHECK(r.ok("measure", J(R"({"kind":"distance","points":[[0,0],[3,4]]})"))
              .find("distance")
              ->asNumber() == 5);
    Json a = r.ok("add_entities", J(R"({"entities":[
        {"kind":"polyline","vertices":[[0,0],[10,0],[10,10],[0,10]],"closed":true},
        {"kind":"circle","center":[0,0],"radius":1}]})"));
    auto ids = a.find("ids")->asArray();
    CHECK(r.ok("measure", J(R"({"kind":"area","ids":[")" + ids[0].asString() + R"("]})"))
              .find("area")
              ->asNumber() == doctest::Approx(100));
    CHECK(r.ok("measure", J(R"({"kind":"area","ids":[")" + ids[1].asString() + R"("]})"))
              .find("area")
              ->asNumber() == doctest::Approx(kPi));
    CHECK(r.ok("measure", J(R"({"kind":"length","ids":[")" + ids[0].asString() + R"("]})"))
              .find("length")
              ->asNumber() == doctest::Approx(40));
    CHECK(r.ok("measure", J(R"({"kind":"area","points":[[0,0],[4,0],[4,3]]})"))
              .find("area")
              ->asNumber() == doctest::Approx(6));
}

TEST_CASE("save and open round trip") {
    auto dir = std::filesystem::temp_directory_path() / "tbcad_mcp_save";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    auto file = dir / "a.tbcad";
    std::string dumpA;
    {
        Rig r;
        r.initialize();
        r.ok("add_entities", fourWalls());
        std::string e = r.fails("save_drawing");
        CHECK(e.find("no file") != std::string::npos);
        e = r.fails("save_drawing", J(R"({"path":"rel.tbcad"})"));
        CHECK(e.find("absolute") != std::string::npos);
        e = r.fails("save_drawing", Json(Json::Object{{"path", file.string() + ".txt"}}));
        CHECK(e.find(".tbcad") != std::string::npos);
        r.ok("save_drawing", Json(Json::Object{{"path", file.string()}}));
        CHECK(std::filesystem::exists(file));
        CHECK_FALSE(r.session.file().modified);
        r.ok("save_drawing", Json(Json::Object{{"path", (dir / "b.tbcad").string()}}));
        e = r.fails("save_drawing", Json(Json::Object{{"path", file.string()}}));
        CHECK(e.find("overwrite") != std::string::npos);
        r.ok("save_drawing", Json(Json::Object{{"path", file.string()}, {"overwrite", true}}));
        dumpA = r.dumpDoc();
    }
    Rig r2;
    r2.initialize();
    r2.ok("open_drawing", Json(Json::Object{{"path", file.string()}}));
    CHECK(r2.dumpDoc() == dumpA);
    CHECK(r2.ok("get_drawing_info").find("name")->asString() == "a.tbcad");

    // Unsaved changes block open/new unless discarded.
    r2.ok("add_entities", J(R"({"entities":[{"kind":"point","position":[0,0]}]})"));
    r2.fails("new_drawing");
    r2.ok("new_drawing", J(R"({"discard_changes":true})"));
    CHECK(r2.session.document().modelSpaceCount() == 0);
    std::filesystem::remove_all(dir);
}

TEST_CASE("a failed tool call leaves no undo entry") {
    Rig r;
    r.initialize();
    r.ok("add_entities", J(R"({"entities":[{"kind":"point","position":[0,0]}]})"));
    std::size_t n = r.session.undoStack().undoCount();
    r.fails("modify_entities", J(R"({"ids":["1"],"set":{"layer":"missing"}})"));
    CHECK(r.session.undoStack().undoCount() == n);
}

TEST_CASE("Active Only mode: MCP edits are limited to the current layer's branch") {
    Rig r;
    r.initialize();
    Json added = r.ok("add_entities", fourWalls());
    auto ids = added.find("ids")->asArray();
    r.ok("set_active_only", J(R"({"on":true})"));  // the current layer is still "0"

    std::string e = r.fails("modify_entities", J(R"({"ids":[")" + ids[0].asString() +
                                                 R"("],"transform":{"move":[1,1]}})"));
    CHECK(e.find("Active Only") != std::string::npos);
    r.fails("delete_entities", J(R"({"ids":[")" + ids[0].asString() + R"("]})"));
    r.fails("add_entities",
            J(R"({"entities":[{"kind":"point","position":[0,0]}],"layer":"A-WALL"})"));
    CHECK(r.session.document().modelSpaceCount() == 4);

    // Making the layer current activates it.
    r.ok("set_current_layer", J(R"({"path":"A-WALL"})"));
    r.ok("modify_entities",
         J(R"({"ids":[")" + ids[0].asString() + R"("],"transform":{"move":[1,1]}})"));
    r.ok("delete_entities", J(R"({"ids":[")" + ids[1].asString() + R"("]})"));
    r.ok("set_active_only", J(R"({"on":false})"));
}

TEST_CASE("levels: create, draw on a level, same position on another level, dim view protects") {
    Rig r;
    r.initialize();
    Json first = r.ok("list_levels", J("{}"));
    CHECK(first.find("levels")->asArray().size() == 1);
    r.ok("create_level", J(R"({"name":"Floor 2","elevation":3000,"make_current":false})"));
    r.fails("create_level", J(R"({"name":"floor 2"})"));
    Json one = r.ok("add_entities", J(R"({"entities":[{"kind":"point","position":[1,1]}]})"));
    r.ok("set_current_level", J(R"({"name":"Floor 2"})"));
    // The same position, on the new level.
    r.ok("add_entities", J(R"({"entities":[{"kind":"point","position":[1,1]}]})"));
    r.ok("add_entities",
         J(R"({"entities":[{"kind":"point","position":[1,1],"level":"Level 1"}]})"));
    const Document& doc = r.session.document();
    int onFloor2 = 0, onFirst = 0;
    doc.forEachEntity([&](const Entity& e) {
        onFloor2 += doc.level(e.common.level)->name == "Floor 2";
        onFirst += e.common.level == kLevelDefault;
    });
    CHECK(onFloor2 == 1);
    CHECK(onFirst == 2);

    // The entity on the other level is read-only in the dim view.
    std::string id = one.find("ids")->asArray()[0].asString();
    std::string e = r.fails("delete_entities", J(R"({"ids":[")" + id + R"("]})"));
    CHECK(e.find("level") != std::string::npos);
    r.ok("set_level_view", J(R"({"mode":"all"})"));
    r.ok("delete_entities", J(R"({"ids":[")" + id + R"("]})"));

    Json q = r.ok("query_entities", J(R"({"levels":["Floor 2"]})"));
    CHECK(q.find("total")->asNumber() == 1);
    r.fails("query_entities", J(R"({"levels":["nope"]})"));

    r.fails("delete_level", J(R"({"name":"Level 1"})"));
    Json del = r.ok("delete_level", J(R"({"name":"Floor 2"})"));
    CHECK(del.find("entitiesMoved")->asNumber() == 1);
    CHECK(r.session.document().levels().size() == 1);
    CHECK(r.session.document().header().currentLevel == kLevelDefault);
}
