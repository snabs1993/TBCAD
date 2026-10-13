#include "rig.h"

using namespace tbcad;
using namespace tbcad::mcp;
using namespace tbcad::mcp::test;

TEST_CASE("json-rpc: malformed input") {
    auto code = [](std::string_view text) {
        ParseOutcome p = parseMessage(text);
        REQUIRE(p.error.has_value());
        return static_cast<int>(p.error->find("error")->find("code")->asNumber());
    };
    CHECK(code("{not json") == rpc::kParseError);
    CHECK(code("42") == rpc::kInvalidRequest);
    CHECK(code("{\"id\":1,\"method\":\"x\"}") == rpc::kInvalidRequest);              // no jsonrpc
    CHECK(code("{\"jsonrpc\":\"2.0\",\"id\":{},\"method\":\"x\"}") == rpc::kInvalidRequest);
    CHECK(code("[{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"ping\"}]") == rpc::kInvalidRequest);
}

TEST_CASE("json-rpc: ids echo as given, notifications get no reply") {
    Rig r;
    std::string reply = r.core.handleText(r.client, "{\"jsonrpc\":\"2.0\",\"id\":\"abc\",\"method\":\"ping\"}");
    CHECK(J(reply).find("id")->asString() == "abc");
    reply = r.core.handleText(r.client, "{\"jsonrpc\":\"2.0\",\"id\":7,\"method\":\"ping\"}");
    CHECK(J(reply).find("id")->asNumber() == 7);
    CHECK(r.core.handleText(r.client, "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/cancelled\"}").empty());
    // A response from the client is ignored.
    CHECK(r.core.handleText(r.client, "{\"jsonrpc\":\"2.0\",\"id\":3,\"result\":{}}").empty());
}

TEST_CASE("json-rpc: unknown method and bad params") {
    Rig r;
    r.initialize();
    Json e = r.rpc("nope/nothing");
    CHECK(e.find("error")->find("code")->asNumber() == rpc::kMethodNotFound);
    e = r.rpc("tools/call", J("{}"));
    CHECK(e.find("error")->find("code")->asNumber() == rpc::kInvalidParams);
    e = r.rpc("tools/call", J("{\"name\":\"no_such_tool\"}"));
    CHECK(e.find("error")->find("code")->asNumber() == rpc::kInvalidParams);
    e = r.rpc("resources/read", J("{}"));
    CHECK(e.find("error")->find("code")->asNumber() == rpc::kInvalidParams);
}

TEST_CASE("lifecycle: protocol negotiation") {
    for (const char* v : {"2025-06-18", "2025-03-26"}) {
        Rig r;
        Json res = *r.rpc("initialize", J(std::string("{\"protocolVersion\":\"") + v +
                                         "\",\"clientInfo\":{\"name\":\"c\",\"version\":\"1\"}}"))
                        .find("result");
        CHECK(res.find("protocolVersion")->asString() == v);
        CHECK(res.find("capabilities")->find("tools")->find("listChanged")->asBool());
        CHECK(res.find("serverInfo")->find("name")->asString() == "tbcad");
        CHECK(res.find("instructions")->asString().size() < 1024);
        CHECK(r.client.info.name == "c");
    }
    Rig r;
    Json res = *r.rpc("initialize", J("{\"protocolVersion\":\"1999-01-01\"}")).find("result");
    CHECK(res.find("protocolVersion")->asString() == "2025-06-18");
}

TEST_CASE("lifecycle: requests before initialize fail, ping works") {
    Rig r;
    CHECK(r.rpc("ping").find("result"));
    Json e = r.rpc("tools/list");
    REQUIRE(e.find("error"));
    CHECK(e.find("error")->find("message")->asString().find("initialize") != std::string::npos);
}

TEST_CASE("tools/list: every tool has a schema and annotations") {
    Rig r;
    r.initialize();
    Json res = *r.rpc("tools/list").find("result");
    const auto& tools = res.find("tools")->asArray();
    CHECK(tools.size() >= 20);
    std::set<std::string> names;
    for (const auto& t : tools) {
        names.insert(t.find("name")->asString());
        CHECK_FALSE(t.find("description")->asString().empty());
        CHECK(t.find("inputSchema")->find("type")->asString() == "object");
        CHECK(t.find("annotations")->find("readOnlyHint"));
        CHECK(t.find("outputSchema"));
    }
    for (const char* n : {"get_drawing_info", "list_layers", "query_entities", "add_entities",
                          "modify_entities", "delete_entities", "run_command", "undo", "redo",
                          "save_drawing", "create_layer", "modify_layer", "delete_layer"})
        CHECK(names.count(n));
    // A headless session has no viewport and no export commands.
    CHECK_FALSE(names.count("capture_view"));
    CHECK_FALSE(names.count("set_view"));
    CHECK_FALSE(names.count("export_pdf"));
}

TEST_CASE("resources: info, layers, commands and the format doc") {
    Rig r;
    r.initialize();
    Json list = *r.rpc("resources/list").find("result");
    CHECK(list.find("resources")->asArray().size() == 4);
    for (const char* uri : {"tbcad://drawing/info", "tbcad://drawing/layers", "tbcad://commands",
                            "tbcad://docs/entity-json"}) {
        Json res = *r.rpc("resources/read", J(std::string("{\"uri\":\"") + uri + "\"}")).find("result");
        CHECK_FALSE(res.find("contents")->asArray()[0].find("text")->asString().empty());
    }
    CHECK(r.rpc("resources/read", J("{\"uri\":\"tbcad://nothing\"}")).find("error"));
}

TEST_CASE("schema validation reports a readable error") {
    Rig r;
    r.initialize();
    std::string e = r.fails("add_entities", J("{}"));
    CHECK(e.find("entities") != std::string::npos);
    e = r.fails("query_entities", J("{\"limit\":0}"));
    CHECK(e.find("limit") != std::string::npos);
    e = r.fails("query_entities", J("{\"kinds\":[1]}"));
    CHECK(e.find("kinds[0]") != std::string::npos);
}
