// The MCP tools against the drawing tabs of the program: new_drawing and open_drawing replace the
// current tab, save_drawing does not put one file in two tabs, create_block works while the
// window's dialogs are on.

#include <doctest/doctest.h>

#include <filesystem>

#include "tbcad/cmd/register_all.h"
#include "tbcad/mcp/json_rpc.h"
#include "tbcad/mcp/protocol.h"
#include "tbcad/mcp/tools.h"
#include "tbcad/mcpapp/controller.h"
#include "tbcad/platform/json.h"
#include "tbcad/platform/paths.h"

using namespace tbcad;
using namespace tbcad::mcpapp;
namespace fs = std::filesystem;
using mcp::dump;
using platform::Json;

namespace {

struct Tabs {
    fs::path dir;
    cmd::Registry registry;
    std::unique_ptr<ui::AppCore> core;
    std::unique_ptr<McpController> mcp;
    std::unique_ptr<mcp::McpServerCore> server;
    mcp::Client client{};
    int nextId = 1;

    explicit Tabs(const std::string& name) {
        dir = fs::temp_directory_path() / ("tbcad_mcptabs_" + name);
        fs::remove_all(dir);
        fs::create_directories(dir);
        registerAllCommands(registry);
        core =
            std::make_unique<ui::AppCore>(registry, platform::AppPaths{dir / "cfg", dir / "data"});
        core->setViewportSize(800, 600);
        core->setDialogsAvailable(true);  // as in the window
        core->settings().setString(kSettingEditPermission, "allow");
        McpController::Hooks hooks;
        mcp = std::make_unique<McpController>(*core, hooks, "test");
        mcp->registerCommands(registry);
        mcp::ToolSet tools;
        mcp::registerDrawingTools(tools);
        // Tool calls run here, on the owner thread, so the session runs them inline.
        server = std::make_unique<mcp::McpServerCore>(mcp->session(), std::move(tools));
        rpc("initialize", json(R"({"protocolVersion":"2025-06-18","capabilities":{},
            "clientInfo":{"name":"tabs","version":"1"}})"));
        mcp::Message n;
        n.kind = mcp::Message::Kind::Notification;
        n.method = "notifications/initialized";
        server->handle(client, n);
    }
    ~Tabs() {
        server.reset();
        mcp.reset();
        core.reset();
        std::error_code ec;
        fs::remove_all(dir, ec);
    }

    static Json json(const std::string& text) {
        auto p = platform::parseJson(text);
        REQUIRE_MESSAGE(p.ok(), text);
        return p.value();
    }
    Json rpc(const std::string& method, Json params) {
        Json::Object m;
        m["jsonrpc"] = "2.0";
        m["id"] = nextId++;
        m["method"] = method;
        m["params"] = std::move(params);
        mcp::ParseOutcome p = mcp::parseMessage(Json(std::move(m)));
        REQUIRE(p.message.has_value());
        auto reply = server->handle(client, *p.message);
        REQUIRE(reply.has_value());
        return *reply;
    }
    Json call(const std::string& tool, const std::string& args) {
        Json r = rpc("tools/call", Json(Json::Object{{"name", tool}, {"arguments", json(args)}}));
        const Json* res = r.find("result");
        REQUIRE_MESSAGE(res, dump(r));
        return *res;
    }
    Json ok(const std::string& tool, const std::string& args = "{}") {
        Json res = call(tool, args);
        INFO(dump(res));
        REQUIRE_FALSE(res.find("isError")->asBool());
        return *res.find("structuredContent");
    }
    std::string fails(const std::string& tool, const std::string& args = "{}") {
        Json res = call(tool, args);
        INFO(dump(res));
        REQUIRE(res.find("isError")->asBool());
        return res.find("content")->asArray()[0].find("text")->asString();
    }
    std::string addLine(double y) {
        Json r =
            ok("add_entities", R"({"entities":[{"kind":"line","start":[0,)" + std::to_string(y) +
                                   R"(],"end":[1000,)" + std::to_string(y) + "]}]}");
        return r.find("ids")->asArray()[0].asString();
    }
    std::string pathArg(const fs::path& p) const { return dump(Json(platform::pathToUtf8(p))); }
};

}  // namespace

TEST_CASE("new_drawing replaces the drawing of the current tab instead of adding tabs") {
    Tabs t("new");
    for (int i = 0; i < 6; ++i) {
        CAPTURE(i);
        t.ok("new_drawing", R"({"discard_changes":true})");
        t.addLine(i * 100.0);
        CHECK(t.core->documentCount() == 1);
        CHECK(t.core->document().modelSpaceCount() == 1);
    }
    // Unsaved changes are still guarded without discard_changes.
    CHECK(t.fails("new_drawing", "{}").find("unsaved") != std::string::npos);
    CHECK(t.core->documentCount() == 1);
}

TEST_CASE("open_drawing replaces the current tab - the user's other tabs stay") {
    Tabs t("open");
    t.addLine(0);
    const fs::path one = t.dir / "one.dxf", two = t.dir / "two.dxf", three = t.dir / "three.dxf";
    t.ok("save_drawing", "{\"path\":" + t.pathArg(one) + "}");
    t.core->newDocument();  // the user's second tab
    t.addLine(10);
    t.ok("save_drawing", "{\"path\":" + t.pathArg(two) + "}");
    t.core->newDocument();  // a third tab, where the AI works
    t.addLine(20);
    REQUIRE(t.core->documentCount() == 3);
    // two.dxf is open already: its tab is shown and the AI's untitled tab goes away.
    t.ok("open_drawing", "{\"path\":" + t.pathArg(two) + ",\"discard_changes\":true}");
    CHECK(t.core->documentCount() == 2);
    CHECK(fs::equivalent(t.core->file(), two));
    // A file that is not open replaces the current tab too.
    t.ok("new_drawing", "{}");
    CHECK(t.core->documentCount() == 2);
    t.addLine(30);
    t.ok("save_drawing", "{\"path\":" + t.pathArg(three) + "}");
    t.ok("new_drawing", "{}");
    t.addLine(40);
    t.ok("open_drawing", "{\"path\":" + t.pathArg(three) + ",\"discard_changes\":true}");
    CHECK(t.core->documentCount() == 2);
    CHECK(fs::equivalent(t.core->file(), three));
    CHECK(fs::equivalent(t.core->documentFile(0), one));
}

TEST_CASE("save_drawing refuses a file that is open in another tab") {
    Tabs t("save");
    t.addLine(0);
    const fs::path file = t.dir / "X.dxf";
    t.ok("save_drawing", "{\"path\":" + t.pathArg(file) + "}");
    // A second tab (the user's New), with its own work.
    t.core->newDocument();
    t.addLine(50);
    REQUIRE(t.core->documentCount() == 2);
    // The file went away outside the program; the first tab still has it open.
    fs::remove(file);
    const std::string why =
        t.fails("save_drawing", "{\"path\":" + t.pathArg(file) + ",\"overwrite\":true}");
    CHECK(why.find("open in another tab") != std::string::npos);
    CHECK(t.core->documentCount() == 2);
    for (std::size_t i = 0; i < t.core->documentCount(); ++i)
        CHECK(t.core->documentTitle(i).find("Read-only") == std::string::npos);
    CHECK_FALSE(t.core->hasFile());
    // Another name works.
    t.ok("save_drawing", "{\"path\":" + t.pathArg(t.dir / "Y.dxf") + "}");
    CHECK(t.core->documentTitle(t.core->activeDocument()).find("Read-only") == std::string::npos);
}

TEST_CASE("create_block works in the window for retain, convert and delete") {
    for (const char* mode : {"retain", "convert", "delete"}) {
        CAPTURE(mode);
        Tabs t(std::string("block_") + mode);
        const std::string a = t.addLine(0);
        const std::string b = t.addLine(100);
        Json made =
            t.ok("create_block", std::string("{\"name\":\"TESTB\",\"ids\":[\"") + a + "\",\"" + b +
                                     "\"],\"base\":[0,0],\"originals\":\"" + mode + "\"}");
        CHECK(t.core->document().blockByName("TESTB") != nullptr);
        const std::size_t left = t.core->document().modelSpaceCount();
        if (std::string(mode) == "retain")
            CHECK(left == 2);
        else if (std::string(mode) == "convert")
            CHECK(left == 1);
        else
            CHECK(left == 0);
        CHECK_FALSE(t.core->runner().active());
    }
}
