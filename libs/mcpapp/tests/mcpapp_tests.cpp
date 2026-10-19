#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <thread>

#include "tbcad/cmd/register_all.h"
#include "tbcad/mcp/http_client.h"
#include "tbcad/mcpapp/controller.h"
#include "tbcad/platform/json.h"

using namespace tbcad;
using namespace tbcad::mcpapp;
namespace fs = std::filesystem;

namespace {

struct Fixture {
    fs::path dir;
    cmd::Registry registry;
    std::unique_ptr<ui::AppCore> core;
    std::unique_ptr<McpController> mcp;

    explicit Fixture(const std::string& name, const std::string& policy = "allow") {
        dir = fs::temp_directory_path() / ("tbcad_mcpapp_" + name);
        fs::remove_all(dir);
        fs::create_directories(dir);
        registerAllCommands(registry);
        core =
            std::make_unique<ui::AppCore>(registry, platform::AppPaths{dir / "cfg", dir / "data"});
        core->setViewportSize(800, 600);
        core->settings().setString(kSettingEditPermission, policy);
        core->settings().setNumber(kSettingPort, 0.0);  // invalid: falls back, then any free port
        McpController::Hooks hooks;
        mcp = std::make_unique<McpController>(*core, hooks, "test");
        mcp->registerCommands(registry);
    }

    // Runs `client` on its own thread while this (the owner) thread pumps. `onPump` runs on the
    // owner thread before each pump, for changes the owner makes (settings are not thread-safe).
    template <class F, class G = void (*)()>
    void withClient(F&& client, G&& onPump = +[] {}) {
        std::atomic<bool> done{false};
        std::thread t([&] {
            client();
            done = true;
        });
        const auto limit = std::chrono::steady_clock::now() + std::chrono::seconds(20);
        while (!done && std::chrono::steady_clock::now() < limit) {
            onPump();
            mcp->pump();
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        t.join();
        mcp->pump();
    }
};

struct Rpc {
    mcp::HttpClient http;
    std::string token;
    std::string session;
    int id = 1;
    Rpc(int port, std::string tok) : http(port), token(std::move(tok)) {}

    std::string post(const std::string& body) {
        std::map<std::string, std::string> h{{"Authorization", "Bearer " + token},
                                             {"Content-Type", "application/json"},
                                             {"Accept", "application/json, text/event-stream"}};
        if (!session.empty())
            h["Mcp-Session-Id"] = session;
        auto r = http.request("POST", "/mcp", h, body);
        REQUIRE(r.ok());
        if (r.value().headers.count("mcp-session-id"))
            session = r.value().headers["mcp-session-id"];
        return r.value().body;
    }
    void init() {
        post(
            R"({"jsonrpc":"2.0","id":0,"method":"initialize","params":{"protocolVersion":"2025-06-18","clientInfo":{"name":"Test AI","version":"1"}}})");
        post(R"({"jsonrpc":"2.0","method":"notifications/initialized"})");
    }
    std::string call(const std::string& tool, const std::string& args) {
        return post(R"({"jsonrpc":"2.0","id":)" + std::to_string(id++) +
                    R"(,"method":"tools/call","params":{"name":")" + tool + R"(","arguments":)" +
                    args + "}}");
    }
};

}  // namespace

TEST_CASE(
    "controller: the mcp.enabled setting starts and stops the server and its discovery file") {
    Fixture f("enable");
    CHECK_FALSE(f.mcp->running());
    f.core->settings().setBool(kSettingEnabled, true);
    f.mcp->pump();
    REQUIRE(f.mcp->running());
    const fs::path disc = mcp::discoveryPath(f.dir / "data");
    auto d = mcp::readDiscovery(disc);
    REQUIRE(d.has_value());
    CHECK(d->url == f.mcp->url());
    CHECK(d->token == f.mcp->token());
    CHECK(f.mcp->url().rfind("http://127.0.0.1:", 0) == 0);
    f.mcp->pump();  // the "listening" line reaches the history
    CHECK(f.core->history().back().find("MCP server listening") != std::string::npos);

    f.core->settings().setBool(kSettingEnabled, false);
    f.mcp->pump();
    CHECK_FALSE(f.mcp->running());
    CHECK_FALSE(mcp::readDiscovery(disc).has_value());
}

TEST_CASE("MCPTOGGLE command and the AI edit: one undo step, history line, activity log") {
    Fixture f("edit");
    f.core->submitText("MCPTOGGLE");
    REQUIRE(f.mcp->running());
    CHECK(f.core->settings().getBool(kSettingEnabled));

    const std::size_t before = f.core->undoStack().undoCount();
    f.withClient([&] {
        Rpc rpc(f.mcp->server()->port(), f.mcp->token());
        rpc.init();
        std::string out = rpc.call(
            "add_entities",
            R"({"entities":[{"kind":"line","start":[0,0],"end":[4000,0]},{"kind":"line","start":[4000,0],"end":[4000,3000]}]})");
        CHECK(out.find("\"isError\":true") == std::string::npos);
        out = rpc.call("get_drawing_info", "{}");
        CHECK(out.find("entityCount") != std::string::npos);
    });
    CHECK(f.core->document().modelSpaceCount() == 2);
    CHECK(f.core->undoStack().undoCount() == before + 1);
    CHECK(f.core->undoLabel() == "Undo AI: add_entities");
    // The scene follows the document without any extra call.
    CHECK(f.core->scene().entityCount() == 2);
    bool sawLine = false;
    for (const auto& h : f.core->history())
        if (h.rfind("[AI: Test AI] add_entities", 0) == 0)
            sawLine = true;
    CHECK(sawLine);
    const auto log = f.mcp->activity();
    REQUIRE(log.size() >= 2);
    CHECK(log.back().call.tool == "get_drawing_info");

    CHECK(f.core->undo());
    CHECK(f.core->document().modelSpaceCount() == 0);
}

TEST_CASE("edit permission: ask can be denied, readOnly refuses edits but allows reads") {
    {
        Fixture f("ask", "ask");
        f.core->settings().setBool(kSettingEnabled, true);
        f.mcp->pump();
        REQUIRE(f.mcp->running());
        std::atomic<bool> stop{false};
        // The "user": waits for the dialog request and denies it.
        std::thread user([&] {
            while (!stop) {
                if (auto r = f.mcp->pendingRequest()) {
                    CHECK(r->client.name == "Test AI");
                    f.mcp->answerRequest(r->id, mcp::EditDecision::Deny);
                    return;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
        });
        f.withClient([&] {
            Rpc rpc(f.mcp->server()->port(), f.mcp->token());
            rpc.init();
            std::string out = rpc.call(
                "add_entities", R"({"entities":[{"kind":"line","start":[0,0],"end":[1,1]}]})");
            CHECK(out.find("\"isError\":true") != std::string::npos);
        });
        stop = true;
        user.join();
        CHECK(f.core->document().modelSpaceCount() == 0);
    }
    {
        Fixture f("readonly", "readOnly");
        f.core->settings().setBool(kSettingEnabled, true);
        f.mcp->pump();
        f.withClient([&] {
            Rpc rpc(f.mcp->server()->port(), f.mcp->token());
            rpc.init();
            CHECK(rpc.call("add_entities",
                           R"({"entities":[{"kind":"line","start":[0,0],"end":[1,1]}]})")
                      .find("\"isError\":true") != std::string::npos);
            CHECK(rpc.call("get_drawing_info", "{}").find("\"isError\":true") == std::string::npos);
        });
        CHECK(f.core->document().modelSpaceCount() == 0);
    }
}

TEST_CASE("stopping the server answers a waiting permission request") {
    Fixture f("stopask", "ask");
    f.core->settings().setBool(kSettingEnabled, true);
    f.mcp->pump();
    std::atomic<bool> stopWanted{false};
    f.withClient(
        [&] {
            Rpc rpc(f.mcp->server()->port(), f.mcp->token());
            rpc.init();
            std::thread stopper([&] {
                while (!f.mcp->pendingRequest())
                    std::this_thread::sleep_for(std::chrono::milliseconds(2));
                stopWanted = true;  // the owner thread switches the setting off in its loop
            });
            rpc.call("add_entities", R"({"entities":[{"kind":"line","start":[0,0],"end":[1,1]}]})");
            stopper.join();
        },
        [&] {
            if (stopWanted.exchange(false))
                f.core->settings().setBool(kSettingEnabled, false);
        });
    CHECK_FALSE(f.mcp->running());
    CHECK(f.core->document().modelSpaceCount() == 0);
}

TEST_CASE("copy texts name the bridge executable") {
    Fixture f("texts");
    McpController::Hooks hooks;
    hooks.bridgeExe = fs::path("C:/Program Files/TBCAD/tbcad-mcp.exe");
    McpController c(*f.core, hooks, "test");
    CHECK(c.claudeCodeCommand().find("claude mcp add tbcad --") == 0);
    CHECK(c.claudeDesktopConfig().find("mcpServers") != std::string::npos);
    CHECK(c.claudeDesktopConfig().find("tbcad-mcp.exe") != std::string::npos);
}
