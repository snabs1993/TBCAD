#include "tbcad/mcp/stdio.h"

#include <istream>
#include <ostream>
#include <string>

namespace tbcad::mcp {

void runStdioLoop(std::istream& in, std::ostream& out, McpServerCore& core, std::ostream* log) {
    Client client;
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (line.empty())
            continue;
        std::string reply = core.handleText(client, line);
        if (!reply.empty()) {
            out << reply << '\n';
            out.flush();
        }
        if (log)
            *log << "mcp: handled " << line.substr(0, 80) << '\n';
    }
}

}  // namespace tbcad::mcp
