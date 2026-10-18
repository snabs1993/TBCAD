// The AI tools on a drawing the size of a big site plan: modify and delete everything, then undo
// and redo, each with the next frame. Size: TBCAD_BENCH_N (default 100,000 polylines + lines).
#include <doctest/doctest.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>

#include "tbcad/cmd/register_all.h"
#include "tbcad/mcp/owner_queue.h"
#include "tbcad/mcp/tools.h"
#include "tbcad/mcpapp/app_session.h"
#include "tbcad/platform/json.h"

using namespace tbcad;
using platform::Json;

namespace {

double ms(std::chrono::steady_clock::time_point t) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t).count();
}

}  // namespace

TEST_CASE("perf: AI tools on a whole big drawing, with undo and redo") {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    const auto dir = std::filesystem::temp_directory_path() / "tbcad_mcp_bulk_perf";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    cmd::Registry registry;
    registerAllCommands(registry);
    ui::AppCore core(registry, platform::AppPaths{dir / "cfg", dir / "data"});
    core.setViewportSize(1400, 900);
    const char* env = std::getenv("TBCAD_BENCH_N");
    const int n = env ? std::atoi(env) : 100000;
    {
        auto tx = core.document().begin("gen");
        for (int i = 0; i < n; ++i) {
            const double x = (i % 500) * 30.0, y = (i / 500) * 30.0;
            PolylineData p;
            for (int k = 0; k < 8; ++k)
                p.v.push_back({{x + k, y + k % 3}, 0, 0, 0});
            Entity e;
            e.data = std::move(p);
            (void)tx.add(std::move(e));
            (void)tx.add(Entity{{}, LineData{{x, y}, {x + 20, y + 10}}});
        }
        tx.commit();
    }
    core.zoomExtents();
    core.updateFrame();

    mcp::OwnerQueue queue;
    mcpapp::AppSession session(core, queue, {});
    mcp::ToolSet tools;
    mcp::registerDrawingTools(tools);
    const mcp::ClientInfo client;
    const std::string version = "test";
    mcp::ToolContext ctx{session, client, version};

    Json::Array idList;
    for (EntityId id : core.document().modelSpace())
        idList.emplace_back(std::to_string(id.value));
    const Json ids(std::move(idList));
    std::printf("AI tools on %zu objects\n", core.document().modelSpaceCount());

    auto call = [&](const char* name, Json args) {
        const mcp::Tool* tool = tools.find(name);
        REQUIRE(tool);
        auto t = std::chrono::steady_clock::now();
        mcp::ToolResult r = tool->handler(ctx, args);
        core.updateFrame();
        const double run = ms(t);
        CHECK_MESSAGE(!r.isError, r.error);
        t = std::chrono::steady_clock::now();
        const bool undone = core.undo();
        core.updateFrame();
        const double undoMs = ms(t);
        t = std::chrono::steady_clock::now();
        const bool redone = core.redo();
        core.updateFrame();
        const double redoMs = ms(t);
        std::printf("%-16s run %6.0f ms   undo %6.0f ms   redo %6.0f ms%s\n", name, run, undoMs,
                    redoMs, undone && redone ? "" : "  [undo or redo did not run]");
        core.undo();
        core.updateFrame();
    };
    call(
        "modify_entities",
        Json(Json::Object{
            {"ids", ids},
            {"transform", Json(Json::Object{{"move", Json(Json::Array{Json(5.0), Json(5.0)})}})}}));
    call("modify_entities",
         Json(Json::Object{{"ids", ids}, {"set", Json(Json::Object{{"color", Json(1.0)}})}}));
    call("delete_entities", Json(Json::Object{{"ids", ids}}));
}
