#include <doctest/doctest.h>

#include <cstdlib>
#include <fstream>
#include <sstream>

#include "tbcad/cmd/register_all.h"
#include "tbcad/core/dump.h"
#include "tbcad/io/tbcad.h"
#include "tbcad/mcp/headless.h"
#include "tbcad/mcp/http_server.h"
#include "tbcad/mcp/tools.h"
#include "tbcad/mcpcli/bridge.h"
#include "tbcad/mcpcli/cli.h"

using namespace tbcad;
using namespace tbcad::mcpcli;
using mcp::Json;

namespace {

Json J(const std::string& text) {
    auto p = platform::parseJson(text);
    REQUIRE(p.ok());
    return p.value();
}

std::string callLine(int id, const std::string& tool, const std::string& args) {
    return R"({"jsonrpc":"2.0","id":)" + std::to_string(id) +
           R"(,"method":"tools/call","params":{"name":")" + tool + R"(","arguments":)" +
           mcp::dump(J(args)) + "}}\n";
}

const char* kInit =
    R"({"jsonrpc":"2.0","id":0,"method":"initialize","params":{"protocolVersion":"2025-06-18","clientInfo":{"name":"cli-test","version":"1"}}})"
    "\n"
    R"({"jsonrpc":"2.0","method":"notifications/initialized"})"
    "\n";

std::vector<Json> parseLines(const std::string& text) {
    std::vector<Json> out;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty())
            out.push_back(J(line));
    }
    return out;
}

std::string textOf(const Json& reply) {
    return reply.find("result")->find("content")->asArray()[0].find("text")->asString();
}

Environment testEnv(const std::filesystem::path& dataDir) {
    Environment env;
    env.self = "/opt/tbcad/tbcad-mcp";
    env.dataDir = dataDir;
    env.version = "9.9";
    return env;
}

std::filesystem::path tempDir(const char* name) {
    auto d = std::filesystem::temp_directory_path() / name;
    std::filesystem::remove_all(d);
    std::filesystem::create_directories(d);
    return d;
}

std::string dumpOfFile(const std::filesystem::path& p) {
    io::IoReport report;
    auto doc = io::readTbcad(p, report);
    REQUIRE(doc.ok());
    return canonicalDump(*doc.value());
}

// Runs tbcad-mcp in process and returns everything it wrote to stdout.
std::string runCli(const std::vector<std::string>& args, const std::string& input,
                   const Environment& env, int* code = nullptr) {
    std::istringstream in(input);
    std::ostringstream out, err;
    int c = run(parseArgs(args), env, in, out, err);
    if (code)
        *code = c;
    return out.str();
}

}  // namespace

TEST_CASE("headless: draw, save, reopen in a new run, dumps match") {
    auto dir = tempDir("tbcad_mcp_cli1");
    auto file = dir / "wall.tbcad";
    Environment env = testEnv(dir);

    std::string input = kInit;
    input += callLine(1, "add_entities",
                      R"({"entities":[
        {"kind":"line","start":[0,0],"end":[4000,0]},
        {"kind":"line","start":[4000,0],"end":[4000,3000]},
        {"kind":"line","start":[4000,3000],"end":[0,3000]},
        {"kind":"line","start":[0,3000],"end":[0,0]}],"layer":"A-WALL","create_layers":true})");
    input += callLine(2, "query_entities", R"({"layers":["A-WALL"]})");
    input +=
        callLine(3, "save_drawing", mcp::dump(Json(Json::Object{{"path", Json(file.string())}})));
    auto replies = parseLines(runCli({"--headless"}, input, env));
    REQUIRE(replies.size() == 4);
    CHECK(replies[0].find("result")->find("serverInfo")->find("version")->asString() == "9.9");
    CHECK(replies[2].find("result")->find("structuredContent")->find("total")->asNumber() == 4);
    CHECK_FALSE(replies[3].find("result")->find("isError")->asBool());
    REQUIRE(std::filesystem::exists(file));

    // A second run opens the file and sees the same drawing.
    std::string again = kInit;
    again += callLine(1, "get_drawing_info", "{}");
    auto r2 = parseLines(runCli({"--headless", file.string()}, again, env));
    REQUIRE(r2.size() == 2);
    CHECK(r2[1].find("result")->find("structuredContent")->find("entityCount")->asNumber() == 4);

    // Saving again from the second run gives an identical drawing.
    auto file2 = dir / "copy.tbcad";
    std::string copy = kInit;
    copy +=
        callLine(1, "save_drawing", mcp::dump(Json(Json::Object{{"path", Json(file2.string())}})));
    runCli({"--headless", file.string()}, copy, env);
    CHECK(dumpOfFile(file) == dumpOfFile(file2));

    // Read-only mode refuses edits.
    std::string ro = kInit;
    ro += callLine(1, "add_entities", R"({"entities":[{"kind":"point","position":[0,0]}]})");
    ro += callLine(2, "query_entities", "{}");
    auto r3 = parseLines(runCli({"--headless", "--read-only", file.string()}, ro, env));
    REQUIRE(r3.size() == 3);
    CHECK(r3[1].find("result")->find("isError")->asBool());
    CHECK(r3[2].find("result")->find("structuredContent")->find("total")->asNumber() == 4);
    std::filesystem::remove_all(dir);
}

TEST_CASE("headless: LINE through run_command, and an unfinished command is rolled back") {
    auto dir = tempDir("tbcad_mcp_cli2");
    Environment env = testEnv(dir);
    std::string input = kInit;
    input += callLine(1, "run_command", R"({"command":"LINE","inputs":["0,0","@100,0",""]})");
    input += callLine(2, "get_drawing_info", "{}");
    input += callLine(3, "run_command", R"({"command":"LINE","inputs":["0,0"]})");
    input += callLine(4, "get_drawing_info", "{}");
    auto r = parseLines(runCli({"--headless"}, input, env));
    REQUIRE(r.size() == 5);
    const Json& line = *r[1].find("result");
    INFO(mcp::dump(line));
    CHECK_FALSE(line.find("isError")->asBool());
    CHECK(line.find("structuredContent")->find("createdCount")->asNumber() == 1);
    CHECK(r[2].find("result")->find("structuredContent")->find("entityCount")->asNumber() == 1);
    const Json& bad = *r[3].find("result");
    CHECK(bad.find("isError")->asBool());
    CHECK(textOf(r[3]).find("Specify next point") != std::string::npos);
    CHECK(r[4].find("result")->find("structuredContent")->find("entityCount")->asNumber() == 1);
    std::filesystem::remove_all(dir);
}

TEST_CASE("bridge without a running TBCAD: tools/list works, tools/call explains") {
    auto dir = tempDir("tbcad_mcp_cli3");  // no mcp.json inside
    std::string input = kInit;
    input += R"({"jsonrpc":"2.0","id":1,"method":"tools/list"})"
             "\n";
    input += callLine(2, "add_entities", R"({"entities":[]})");
    input += R"({"jsonrpc":"2.0","id":3,"method":"ping"})"
             "\n";
    auto r = parseLines(runCli({}, input, testEnv(dir)));
    REQUIRE(r.size() == 4);
    CHECK(r[1].find("result")->find("tools")->asArray().size() > 10);
    CHECK(r[2].find("result")->find("isError")->asBool());
    CHECK(textOf(r[2]).find("Tools > MCP Server") != std::string::npos);
    CHECK(r[3].find("result"));
    std::filesystem::remove_all(dir);
}

TEST_CASE("bridge to a running server, and it follows a restart") {
    auto dir = tempDir("tbcad_mcp_cli4");
    auto discovery = mcp::discoveryPath(dir);
    cmd::Registry registry;
    registerAllCommands(registry);
    mcp::HeadlessSession session(registry);
    mcp::ToolSet tools;
    mcp::registerDrawingTools(tools);
    mcp::McpServerCore core(session, std::move(tools));

    auto startServer = [&](std::unique_ptr<mcp::HttpServer>& server) {
        mcp::HttpServerOptions opt;
        opt.port = 0;
        server = std::make_unique<mcp::HttpServer>(core, opt);
        REQUIRE(server->start().ok());
        REQUIRE(mcp::writeDiscovery(discovery, {server->url(), server->token(), 1, "t"}).ok());
    };

    std::ostringstream out;
    BridgeConfig cfg;
    cfg.discoveryFile = discovery;
    Bridge bridge(cfg, out);
    std::unique_ptr<mcp::HttpServer> server;

    // The AI client starts first; TBCAD is not running yet.
    for (
        const char* l :
        {R"({"jsonrpc":"2.0","id":0,"method":"initialize","params":{"protocolVersion":"2025-06-18","clientInfo":{"name":"cli-test","version":"1"}}})"})
        bridge.handleLine(l);
    bridge.handleLine(R"({"jsonrpc":"2.0","method":"notifications/initialized"})");
    bridge.handleLine(callLine(1, "get_drawing_info", "{}"));

    // Now TBCAD starts: the next call goes through without restarting the client.
    startServer(server);
    bridge.handleLine(
        callLine(2, "add_entities", R"({"entities":[{"kind":"point","position":[1,1]}]})"));
    bridge.handleLine(callLine(3, "get_drawing_info", "{}"));
    CHECK(session.document().modelSpaceCount() == 1);
    CHECK(server->sessions().size() == 1);
    CHECK(server->sessions()[0].clientName == "cli-test");

    // TBCAD restarts with a new port and token.
    server->stop();
    server.reset();
    startServer(server);
    bridge.handleLine(
        callLine(4, "add_entities", R"({"entities":[{"kind":"point","position":[2,2]}]})"));
    CHECK(session.document().modelSpaceCount() == 2);

    bridge.shutdown();
    server->stop();
    auto replies = parseLines(out.str());
    REQUIRE(replies.size() == 5);
    CHECK(textOf(replies[1]).find("not running") != std::string::npos);
    CHECK_FALSE(replies[2].find("result")->find("isError")->asBool());
    CHECK(replies[3].find("result")->find("structuredContent")->find("entityCount")->asNumber() ==
          1);
    CHECK_FALSE(replies[4].find("result")->find("isError")->asBool());
    std::filesystem::remove_all(dir);
}

TEST_CASE("command line: options and print-config") {
    CHECK(parseArgs({"--headless", "a.tbcad", "--read-only"}).file->string() == "a.tbcad");
    CHECK(parseArgs({"--headless", "--read-only"}).readOnly);
    CHECK_FALSE(parseArgs({"--read-only"}).error.empty());
    CHECK_FALSE(parseArgs({"--bogus"}).error.empty());
    CHECK(parseArgs({"--launch", "--log", "x.log"}).logFile->string() == "x.log");

    std::string text = runCli({"--print-config"}, "", testEnv("/tmp"));
    CHECK(text.find("mcpServers") != std::string::npos);
    CHECK(text.find("/opt/tbcad/tbcad-mcp") != std::string::npos);
    CHECK(text.find("claude mcp add tbcad --") != std::string::npos);
    int code = 0;
    runCli({"--nonsense"}, "", testEnv("/tmp"), &code);
    CHECK(code == 2);
    CHECK(runCli({"--version"}, "", testEnv("/tmp")) == "tbcad-mcp 9.9\n");
}

TEST_CASE("the tbcad-mcp executable speaks MCP over redirected stdio") {
    auto dir = tempDir("tbcad_mcp_cli5");
    auto in = dir / "in.jsonl";
    auto out = dir / "out.jsonl";
    {
        std::ofstream f(in, std::ios::binary);
        f << kInit << callLine(1, "get_drawing_info", "{}");
    }
    std::string cmd = "\"" + std::string(TBCAD_MCP_EXE) + "\" --headless < \"" + in.string() +
                      "\" > \"" + out.string() + "\"";
#ifdef _WIN32
    cmd = "\"" + cmd + "\"";
#endif
    REQUIRE(std::system(cmd.c_str()) == 0);
    std::ifstream f(out, std::ios::binary);
    std::stringstream buf;
    buf << f.rdbuf();
    auto replies = parseLines(buf.str());
    REQUIRE(replies.size() == 2);
    CHECK(replies[0].find("result")->find("serverInfo")->find("name")->asString() == "tbcad");
    CHECK(replies[1].find("result")->find("structuredContent")->find("units")->asString() == "mm");
    std::filesystem::remove_all(dir);
}
