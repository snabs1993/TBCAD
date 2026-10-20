// libFuzzer target: the MCP server fed hostile JSON-RPC (one message per line) against a fresh
// drawing. Parsing, dispatch and every drawing tool must answer with a result or an error, never
// crash or hang. Tools that touch files (open, save, export) and run_command are left out.
// Build: cmake -DTBCAD_MCP_FUZZ=ON (clang), run tbcad_mcp_fuzz <corpus dir> -max_total_time=30
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>

#include "tbcad/cmd/registry.h"
#include "tbcad/mcp/headless.h"
#include "tbcad/mcp/protocol.h"
#include "tbcad/mcp/tools.h"
#include "tbcad/platform/json.h"

using namespace tbcad;

namespace {

// Tools left out because they read or write files, or run any command (which may too).
bool touchesFiles(std::string_view line) {
    auto msg = platform::parseJson(line);
    if (!msg.ok() || !msg.value().isObject())
        return false;
    const platform::Json* params = msg.value().find("params");
    const platform::Json* name = params && params->isObject() ? params->find("name") : nullptr;
    if (!name || !name->isString())
        return false;
    const std::string& n = name->asString();
    for (std::string_view w : {"save", "open", "export", "run_command", "plot", "image"})
        if (n.find(w) != std::string::npos)
            return true;
    return false;
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    static cmd::Registry* registry = [] {
        auto* r = new cmd::Registry();
        cmd::registerBuiltinCommands(*r);
        return r;
    }();
    mcp::HeadlessSession session(*registry);
    mcp::ToolSet tools;
    mcp::registerDrawingTools(tools);
    mcp::McpServerCore core(session, std::move(tools));
    mcp::Client client;
    (void)core.handleText(client, R"({"jsonrpc":"2.0","id":0,"method":"initialize","params":{)"
                                  R"("protocolVersion":"2025-06-18","capabilities":{},)"
                                  R"("clientInfo":{"name":"fuzz","version":"1"}}})");
    (void)core.handleText(client, R"({"jsonrpc":"2.0","method":"notifications/initialized"})");
    std::string_view text(reinterpret_cast<const char*>(data), size);
    int messages = 0;
    while (!text.empty() && messages++ < 16) {
        const std::size_t nl = text.find('\n');
        const std::string_view line = text.substr(0, nl);
        text = nl == std::string_view::npos ? std::string_view{} : text.substr(nl + 1);
        if (touchesFiles(line))
            continue;
        const std::string reply = core.handleText(client, line);
        if (std::getenv("TBCAD_FUZZ_ECHO"))
            std::fprintf(stderr, "%s\n", reply.c_str());
    }
    return 0;
}
