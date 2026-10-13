#include <fstream>
#include <iostream>

#include "tbcad/cmd/register_all.h"
#include "tbcad/mcp/headless.h"
#include "tbcad/mcp/protocol.h"
#include "tbcad/mcp/stdio.h"
#include "tbcad/mcp/tools.h"
#include "tbcad/mcpcli/bridge.h"
#include "tbcad/mcpcli/cli.h"

namespace tbcad::mcpcli {

std::string configText(const std::filesystem::path& exe) {
    using mcp::Json;
    std::string path = exe.string();
    Json desktop(Json::Object{
        {"mcpServers",
         Json(Json::Object{
             {"tbcad", Json(Json::Object{{"command", path}, {"args", Json(Json::Array{})}})}})}});
    std::string out;
    out += "Claude Desktop (claude_desktop_config.json):\n";
    out += platform::toJson(desktop, true);
    out += "\nClaude Code (stdio, recommended):\n";
    out += "  claude mcp add tbcad -- \"" + path + "\"\n";
    out +=
        "\nClaude Code (HTTP; the token changes every time TBCAD starts the server, so stdio is "
        "the better choice):\n";
    out +=
        "  claude mcp add --transport http tbcad http://127.0.0.1:41730/mcp --header "
        "\"Authorization: Bearer <token from mcp.json>\"\n";
    out +=
        "\nTBCAD must be running with Tools > MCP Server turned on, or use --launch or "
        "--headless <file.tbcad>.\n";
    return out;
}

int run(const Options& options, const Environment& env, std::istream& in, std::ostream& out,
        std::ostream& err) {
    if (!options.error.empty()) {
        err << "tbcad-mcp: " << options.error << "\n\n" << usage();
        return 2;
    }
    if (options.help) {
        out << usage();
        return 0;
    }
    if (options.version) {
        out << "tbcad-mcp " << env.version << "\n";
        return 0;
    }
    if (options.printConfig) {
        out << configText(env.self);
        return 0;
    }

    std::ofstream logFile;
    std::ostream* log = &err;
    if (options.logFile) {
        logFile.open(*options.logFile, std::ios::app);
        if (logFile)
            log = &logFile;
    }

    if (options.headless) {
        cmd::Registry registry;
        registerAllCommands(registry);
        mcp::HeadlessSession session(registry);
        session.setReadOnly(options.readOnly);
        if (options.file) {
            auto r = session.load(*options.file);
            if (!r) {
                err << "tbcad-mcp: cannot open " << options.file->string() << ": "
                    << r.error().message << "\n";
                return 1;
            }
        }
        mcp::ToolSet tools;
        mcp::registerDrawingTools(tools);
        mcp::ServerInfo info;
        info.version = env.version;
        mcp::McpServerCore core(session, std::move(tools), info);
        mcp::runStdioLoop(in, out, core, nullptr);
        return 0;
    }

    BridgeConfig cfg;
    cfg.discoveryFile = mcp::discoveryPath(env.dataDir);
    cfg.log = log;
    if (options.launch)
        cfg.launch = [&env, log] { return launchTbcad(env, *log); };
    Bridge bridge(cfg, out);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (!line.empty())
            bridge.handleLine(line);
    }
    bridge.shutdown();
    return 0;
}

}  // namespace tbcad::mcpcli
