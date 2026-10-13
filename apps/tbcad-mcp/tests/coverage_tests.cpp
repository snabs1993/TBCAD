// The MCP tools against the real command registry: what the AI sees for the commands of the
// latest workstreams. (libs/mcp has its own tests with a stand-in registry.)

#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>

#include "tbcad/cmd/register_all.h"
#include "tbcad/core/dump.h"
#include "tbcad/mcp/headless.h"
#include "tbcad/mcp/protocol.h"
#include "tbcad/mcp/tools.h"

using namespace tbcad;
using namespace tbcad::mcp;

namespace {

Json J(const std::string& text) {
    auto p = platform::parseJson(text);
    REQUIRE_MESSAGE(p.ok(), text);
    return p.value();
}

struct Full {
    cmd::Registry reg{};
    HeadlessSession session;
    McpServerCore core;
    Client client{};
    int nextId = 1;

    static cmd::Registry& prepare(cmd::Registry& r) {
        registerAllCommands(r);
        return r;
    }
    static ToolSet tools() {
        ToolSet t;
        registerDrawingTools(t);
        return t;
    }
    Full() : session(prepare(reg)), core(session, tools()) {
        rpc("initialize", J(R"({"protocolVersion":"2025-06-18","capabilities":{},
            "clientInfo":{"name":"coverage","version":"1"}})"));
        Message n;
        n.kind = Message::Kind::Notification;
        n.method = "notifications/initialized";
        core.handle(client, n);
    }

    Json rpc(const std::string& method, Json params) {
        Json::Object m;
        m["jsonrpc"] = "2.0";
        m["id"] = nextId++;
        m["method"] = method;
        m["params"] = std::move(params);
        ParseOutcome p = parseMessage(Json(std::move(m)));
        REQUIRE(p.message.has_value());
        auto reply = core.handle(client, *p.message);
        REQUIRE(reply.has_value());
        return *reply;
    }
    Json call(const std::string& name, const std::string& args = "{}") {
        Json r = rpc("tools/call", Json(Json::Object{{"name", name}, {"arguments", J(args)}}));
        const Json* res = r.find("result");
        REQUIRE_MESSAGE(res, dump(r));
        return *res;
    }
    Json ok(const std::string& name, const std::string& args = "{}") {
        Json res = call(name, args);
        INFO(dump(res));
        REQUIRE_FALSE(res.find("isError")->asBool());
        return *res.find("structuredContent");
    }
    std::string fails(const std::string& name, const std::string& args = "{}") {
        Json res = call(name, args);
        INFO(dump(res));
        REQUIRE(res.find("isError")->asBool());
        return res.find("content")->asArray()[0].find("text")->asString();
    }
    std::string add(const std::string& entity) {
        return ok("add_entities", "{\"entities\":[" + entity + "]}")
            .find("ids")
            ->asArray()[0]
            .asString();
    }
};

}  // namespace

TEST_CASE("layouts: create, add a viewport and a title block, change it, plot it to PDF") {
    Full f;
    f.ok("add_entities", R"({"entities":[
        {"kind":"line","start":[0,0],"end":[4000,0]},{"kind":"line","start":[4000,0],"end":[4000,3000]},
        {"kind":"line","start":[4000,3000],"end":[0,3000]},{"kind":"line","start":[0,3000],"end":[0,0]}]})");
    Json made = f.ok("create_layout", R"({"name":"Plan A3","paper":"A3"})");
    CHECK(made.find("paper")->asString() == "A3");
    CHECK(made.find("size")->asArray()[0].asNumber() == doctest::Approx(420));
    Json vp = f.ok(
        "add_viewport",
        R"({"layout":"Plan A3","rect":[[10,10],[300,200]],"center":[2000,1500],"scale":"1:20"})");
    int id = static_cast<int>(vp.find("viewport")->asNumber());
    CHECK(id == 1);
    f.ok("add_title_block", R"({"layout":"Plan A3","project":"House","drawing":"Ground floor",
         "scale":"1:20","date":"2026-10-13","sheet":"A-101"})");
    Json changed = f.ok("modify_viewport", R"({"layout":"Plan A3","id":1,"scale":"1:50"})");
    CHECK(changed.find("viewports")->asArray()[0].find("scaleN")->asNumber() == 50);
    f.ok("create_layer", R"({"name":"DIM"})");
    changed = f.ok("modify_viewport", R"({"layout":"Plan A3","id":1,"freeze":["DIM"]})");
    CHECK(changed.find("viewports")->asArray()[0].find("frozenLayers")->asArray()[0].asString() ==
          "DIM");
    Json list = f.ok("list_layouts");
    CHECK(list.find("layouts")->asArray().size() == 1);

    auto dir = std::filesystem::temp_directory_path() / "tbcad_mcp_coverage";
    std::filesystem::create_directories(dir);
    auto pdf = dir / "layout.pdf";
    std::filesystem::remove(pdf);
    f.ok("export_pdf", "{\"path\":\"" + pdf.generic_string() + "\",\"layout\":\"Plan A3\"}");
    REQUIRE(std::filesystem::exists(pdf));
    {
        std::ifstream in(pdf, std::ios::binary);
        std::string all((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        CHECK(all.rfind("%PDF-", 0) == 0);
        // the sheet is A3 landscape: 420 x 297 mm = 1190.55 x 841.89 pt
        CHECK(all.find("1190") != std::string::npos);
    }

    // model space again: a different page size is not required, the call must simply work
    auto pdf2 = dir / "model.pdf";
    std::filesystem::remove(pdf2);
    f.ok("export_pdf", "{\"path\":\"" + pdf2.generic_string() + "\"}");
    CHECK(std::filesystem::exists(pdf2));
    CHECK(f.fails("export_pdf", "{\"path\":\"" + (dir / "x.pdf").generic_string() +
                                    "\",\"layout\":\"No such sheet\"}")
              .size() > 0);

    CHECK(f.fails("add_viewport", R"({"layout":"Nope","rect":[[0,0],[10,10]]})").find("Nope") !=
          std::string::npos);
    f.ok("modify_viewport", R"({"layout":"Plan A3","id":1,"remove":true})");
    f.ok("delete_layout", R"({"name":"Plan A3"})");
    CHECK(f.ok("list_layouts").find("layouts")->asArray().empty());
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

TEST_CASE("blocks: create with an attribute, list its details, insert it") {
    Full f;
    std::string a = f.add(R"({"kind":"line","start":[0,0],"end":[10,0]})");
    std::string b = f.add(R"({"kind":"line","start":[10,0],"end":[10,5]})");
    std::string t = f.add(R"({"kind":"text","position":[1,1],"height":2,"value":"TAG",
        "attdef":{"tag":"mark","prompt":"Mark","default":"A1"}})");
    Json made = f.ok("create_block", "{\"name\":\"Door\",\"ids\":[\"" + a + "\",\"" + b + "\",\"" +
                                         t + "\"],\"base\":[0,0]}");
    CHECK(made.find("name")->asString() == "Door");
    Json list = f.ok("list_blocks");
    bool found = false;
    for (const Json& blk : list.find("blocks")->asArray())
        if (blk.find("name")->asString() == "Door") {
            found = true;
            const Json* attrs = blk.find("attributes");
            REQUIRE(attrs);
            REQUIRE(attrs->asArray().size() == 1);
            CHECK(attrs->asArray()[0].find("tag")->asString() == "MARK");
        }
    CHECK(found);
    std::string c = f.add(R"({"kind":"line","start":[0,0],"end":[1,1]})");
    CHECK(f.fails("create_block", "{\"name\":\"Door\",\"ids\":[\"" + c + "\"]}").find("redefine") !=
          std::string::npos);
    CHECK(f.fails("create_block", R"({"name":"X","ids":[]})").size() > 0);
}

TEST_CASE("layer states: save, change, restore, rename, delete") {
    Full f;
    f.ok("create_layer", R"({"name":"WALLS"})");
    f.ok("layer_state", R"({"action":"save","name":"before"})");
    CHECK(f.ok("layer_state").find("states")->asArray().size() == 1);
    f.ok("layer_state", R"({"action":"restore","name":"before"})");
    f.ok("layer_state", R"({"action":"rename","name":"before","new_name":"after"})");
    CHECK(f.ok("layer_state").find("states")->asArray()[0].asString() == "after");
    f.ok("layer_state", R"({"action":"delete","name":"after"})");
    CHECK(f.ok("layer_state").find("states")->asArray().empty());
    CHECK(f.fails("layer_state", R"({"action":"restore","name":"nope"})").size() > 0);
}

TEST_CASE("constraints: add, list, solve and remove") {
    Full f;
    std::string l1 = f.add(R"({"kind":"line","start":[0,0],"end":[10,1]})");
    std::string l2 = f.add(R"({"kind":"line","start":[0,5],"end":[10,9]})");
    Json r = f.ok("add_constraint", "{\"kind\":\"Horizontal\",\"a\":\"" + l1 + "\"}");
    CHECK(r.find("constraint")->find("kind")->asString() == "Horizontal");
    f.ok("add_constraint", "{\"kind\":\"Parallel\",\"a\":\"" + l1 + "\",\"b\":\"" + l2 + "\"}");
    Json d = f.ok("add_constraint", "{\"kind\":\"Distance\",\"a\":{\"id\":\"" + l1 +
                                        "\",\"part\":\"start\"},\"b\":{\"id\":\"" + l1 +
                                        "\",\"part\":\"end\"},\"value\":25}");
    CHECK(d.find("constraint")->find("value")->asNumber() == doctest::Approx(25));
    Json list = f.ok("list_constraints");
    CHECK(list.find("constraints")->asArray().size() == 3);
    Json e = f.ok("get_entity", "{\"id\":\"" + l1 + "\"}");
    CHECK(e.find("end")->asArray()[0].asNumber() - e.find("start")->asArray()[0].asNumber() ==
          doctest::Approx(25).epsilon(1e-6));
    CHECK(f.fails("add_constraint", "{\"kind\":\"Distance\",\"a\":\"" + l1 + "\"}").size() > 0);
    f.ok("solve_constraints");
    f.ok("remove_constraint", R"({"id":1})");
    CHECK(f.fails("remove_constraint", R"({"id":99})").find("99") != std::string::npos);
    CHECK(f.ok("list_constraints").find("constraints")->asArray().size() == 2);
}

TEST_CASE("dimensions: written as JSON, then chained with DIMBASELINE and DIMCONTINUE") {
    Full f;
    f.add(
        R"({"kind":"dimension","dimKind":"linear","start":[0,0],"end":[3000,0],"dimLine":[1500,-600]})");
    Json r = f.ok("run_command", R"({"command":"DIMBASELINE","inputs":["6000,0",""]})");
    CHECK(r.find("created")->asArray().size() == 1);
    r = f.ok("run_command", R"({"command":"DIMCONTINUE","inputs":["9000,0",""]})");
    CHECK(r.find("created")->asArray().size() == 1);
    Json q = f.ok("query_entities", R"({"kinds":["dimension"]})");
    CHECK(q.find("entities")->asArray().size() == 3);
}

TEST_CASE("hatch gradient and Active Only through the tools") {
    Full f;
    std::string h = f.add(R"({"kind":"hatch","solid":true,
        "loops":[[[0,0],[10,0],[10,10],[0,10]]],
        "gradient":{"type":"cylinder","color1":"#FF0000","color2":"#0000FF","angle":0.5}})");
    Json g = f.ok("get_entity", "{\"id\":\"" + h + "\"}");
    REQUIRE(g.find("gradient"));
    CHECK(g.find("gradient")->find("type")->asString() == "cylinder");
    f.ok("create_layer", R"({"name":"OTHER","make_current":true})");
    f.ok("set_active_only", R"({"on":true})");
    CHECK(f.fails("delete_entities", "{\"ids\":[\"" + h + "\"]}").size() > 0);
    f.ok("set_active_only", R"({"on":false})");
    f.ok("delete_entities", "{\"ids\":[\"" + h + "\"]}");
}

TEST_CASE("dynamic blocks: parameter, action, set value on an insert") {
    Full f;
    std::string a = f.add(R"({"kind":"line","start":[0,0],"end":[10,0]})");
    std::string b = f.add(R"({"kind":"line","start":[10,0],"end":[10,5]})");
    f.ok("create_block", "{\"name\":\"Bar\",\"ids\":[\"" + a + "\",\"" + b + "\"]}");
    CHECK(f.fails("add_block_parameter", R"({"block":"Nope","type":"linear"})").size() > 0);
    f.ok("add_block_parameter",
         R"({"block":"Bar","type":"linear","name":"Length","start":[0,0],"end":[10,0]})");
    CHECK(f.fails("add_block_parameter",
                  R"({"block":"Bar","type":"linear","name":"Length","start":[0,0],"end":[10,0]})")
              .find("already") != std::string::npos);
    f.ok("add_block_action", R"({"block":"Bar","parameter":"Length","action":"stretch",
         "frame":[[5,-1],[11,6]],"objects":[[5,-1],[11,6]]})");
    Json list = f.ok("list_blocks");
    bool seen = false;
    for (const Json& blk : list.find("blocks")->asArray())
        if (blk.find("name")->asString() == "Bar") {
            seen = true;
            CHECK(blk.find("dynamicParameters")->asArray().size() == 1);
            CHECK(blk.find("dynamicActions")->asNumber() == 1);
        }
    CHECK(seen);
    std::string ins = f.add(R"({"kind":"insert","block":"Bar","position":[100,0]})");
    f.ok("set_dynamic_value", "{\"id\":\"" + ins + "\",\"parameter\":\"Length\",\"value\":25}");
    Json got = f.ok("get_entity", "{\"id\":\"" + ins + "\"}");
    REQUIRE(got.find("dynamicValues"));
    CHECK(dump(*got.find("dynamicValues")).find("25") != std::string::npos);
    CHECK(f.fails("set_dynamic_value", "{\"id\":\"" + ins + "\",\"parameter\":\"Zzz\",\"value\":1}")
              .size() > 0);
}
