#include "tbcad/mcpcli/cli.h"

namespace tbcad::mcpcli {

Options parseArgs(const std::vector<std::string>& args) {
    Options o;
    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string& a = args[i];
        if (a == "--headless") {
            o.headless = true;
        } else if (a == "--read-only") {
            o.readOnly = true;
        } else if (a == "--launch") {
            o.launch = true;
        } else if (a == "--log") {
            if (i + 1 >= args.size()) {
                o.error = "--log needs a file name";
                return o;
            }
            o.logFile = std::filesystem::path(args[++i]);
        } else if (a == "--print-config") {
            o.printConfig = true;
        } else if (a == "--help" || a == "-h" || a == "/?") {
            o.help = true;
        } else if (a == "--version") {
            o.version = true;
        } else if (a.rfind("-", 0) != 0 && !o.file) {
            o.file = std::filesystem::path(a);  // the drawing for --headless
        } else {
            o.error = "unknown argument: " + a;
            return o;
        }
    }
    if (o.readOnly && !o.headless)
        o.error = "--read-only only applies to --headless";
    if (o.file && !o.headless && o.error.empty())
        o.error = "a file name only applies to --headless";
    return o;
}

std::string usage() {
    return "tbcad-mcp: MCP server for TBCAD over stdio\n"
           "\n"
           "  tbcad-mcp                      bridge to the running TBCAD (Tools > MCP Server)\n"
           "  tbcad-mcp --launch             same; starts TBCAD if it is not running\n"
           "  tbcad-mcp --headless [file]    edit a .tbcad file without a window\n"
           "  tbcad-mcp --headless --read-only [file]\n"
           "  tbcad-mcp --print-config       show config for Claude Desktop and Claude Code\n"
           "  --log <file>                   write diagnostics to a file (stderr by default)\n"
           "  --version, --help\n"
           "\n"
           "Messages are newline-delimited JSON-RPC on stdin/stdout (MCP 2025-06-18).\n";
}

}  // namespace tbcad::mcpcli
