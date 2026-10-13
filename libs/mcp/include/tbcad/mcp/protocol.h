#pragma once

#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

#include "tbcad/mcp/json_rpc.h"
#include "tbcad/mcp/tool.h"

namespace tbcad::mcp {

inline constexpr const char* kProtocolVersion = "2025-06-18";
inline constexpr const char* kProtocolVersionOld = "2025-03-26";

// State of one MCP session (one initialize handshake). Belongs to the transport that created it.
struct Client {
    bool initialized = false;      // initialize request answered
    bool ready = false;            // notifications/initialized received
    std::string protocol{};
    ClientInfo info{};
    std::optional<EditDecision> decision{};  // answer of requestEdit for this session
    std::string logLevel = "info";
    std::mutex mutex{};
};

// A protocol-level failure (unknown tool ...). Tool failures are results with isError instead.
struct RpcError {
    int code = -32603;
    std::string message{};
};

struct ServerInfo {
    std::string name = "tbcad";
    std::string title = "TBCAD";
    std::string version = "0.0.1";
};

// Protocol core: lifecycle and dispatch for every transport. Thread-safe as long as each Client
// is used by one request at a time (the document work is serialised by the DrawingSession).
class McpServerCore {
public:
    McpServerCore(DrawingSession& session, ToolSet tools, ServerInfo info = {});

    // Handles one parsed message. Returns the JSON-RPC reply, or nothing for notifications and
    // client responses.
    std::optional<Json> handle(Client& client, const Message& message);
    // Parses `text` first; parse errors become replies. Empty string = nothing to send.
    std::string handleText(Client& client, std::string_view text);

    DrawingSession& session() { return session_; }
    const ToolSet& tools() const { return tools_; }
    const ServerInfo& info() const { return info_; }

    // Called when the set of listed tools changes; transports turn it into
    // notifications/tools/list_changed.
    void setToolsChangedSink(std::function<void()> sink) { toolsChanged_ = std::move(sink); }
    void notifyToolsChanged() {
        if (toolsChanged_)
            toolsChanged_();
    }

    // Runs a tool the way tools/call does (validation, permission, undo group). Returns the
    // CallToolResult object; throws RpcError for an unknown tool. Public for tests.
    Json callTool(Client& client, const std::string& name, const Json& arguments);

private:
    Json initialize(Client& client, const Message& m);
    Json listTools();
    Json listResources();
    Json readResource(const Json& params);
    Json serverInstructions() const;
    EditDecision permission(Client& client);

    DrawingSession& session_;
    ToolSet tools_;
    ServerInfo info_;
    std::function<void()> toolsChanged_{};
};

}  // namespace tbcad::mcp
