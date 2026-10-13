#pragma once

#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "tbcad/mcp/json_rpc.h"
#include "tbcad/mcp/session.h"

namespace tbcad::mcp {

struct ToolContext {
    DrawingSession& session;
    const ClientInfo& client;
    const std::string& serverVersion;
};

struct ToolResult {
    Json data{};                 // becomes structuredContent and the text content
    std::optional<Image> image;  // becomes an image content block (PNG)
    std::string error{};         // non-empty: isError result with this text
    bool isError = false;

    static ToolResult ok(Json data) {
        ToolResult r;
        r.data = std::move(data);
        return r;
    }
    static ToolResult fail(std::string message) {
        ToolResult r;
        r.error = std::move(message);
        r.isError = true;
        return r;
    }
};

using ToolHandler = std::function<ToolResult(ToolContext&, const Json& arguments)>;

// What a tool does to the program. Anything but None needs the user's edit permission.
enum class Edit {
    None,      // reads only
    Document,  // changes the drawing: runs inside one undo group labelled "AI: <tool>"
    Session,   // changes something else (selection, view, files, undo history): no group
};

struct Tool {
    std::string name{};
    std::string title{};
    std::string description{};
    Json inputSchema{};
    Json outputSchema{};  // null = none
    Edit edit = Edit::None;
    bool destructive = false;
    bool idempotent = false;
    // Refuse while the user is in the middle of a command. Ignored for Edit::None.
    bool needsIdle = true;
    ToolHandler handler{};
    // Tools whose backing is missing are not listed. Null = always available.
    std::function<bool(DrawingSession&)> available{};
};

class ToolSet {
public:
    void add(Tool tool) { tools_.push_back(std::move(tool)); }
    const Tool* find(const std::string& name) const;
    // Tools available for this session, in registration order.
    std::vector<const Tool*> list(DrawingSession& session) const;
    const std::vector<Tool>& all() const { return tools_; }

private:
    std::vector<Tool> tools_{};
};

}  // namespace tbcad::mcp
