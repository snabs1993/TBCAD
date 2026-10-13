#pragma once

#include <functional>

#include "tbcad/mcp/owner_queue.h"
#include "tbcad/mcp/session.h"
#include "tbcad/ui/app_core.h"

namespace tbcad::mcpapp {

// The drawing the user has open in the program, as seen by the MCP tools. Everything that touches
// AppCore runs on the owner (UI) thread through the OwnerQueue; the tool handlers already wrap
// their work in runOnOwnerThread, so most members here are only called there.
class AppSession final : public mcp::DrawingSession {
public:
    struct Hooks {
        // Renders `area` (empty = the current view) into RGBA pixels; owner thread only.
        std::function<std::optional<mcp::Image>(int maxPx, const BBox2& area)> capture{};
        // Permission for one client; called on a server thread, may block.
        std::function<mcp::EditDecision(const mcp::ClientInfo&)> requestEdit{};
        std::function<void(const mcp::ToolCallLog&)> log{};
    };

    AppSession(ui::AppCore& core, mcp::OwnerQueue& queue, Hooks hooks);

    void runOnOwnerThread(std::function<void()> fn) override { queue_.run(std::move(fn)); }
    Document& document() override { return core_.document(); }
    cmd::UndoStack& undoStack() override { return core_.undoStack(); }
    cmd::Registry& registry() override { return core_.registry(); }
    cmd::CommandRunner& runner() override { return core_.runner(); }
    const index::SpatialIndex* spatialIndex() override { return &core_.spatialIndex(); }
    std::vector<EntityId> selection() override { return core_.selection(); }
    void setSelection(std::vector<EntityId> ids) override { core_.setSelection(std::move(ids)); }
    mcp::FileInfo file() override;
    Result<void> save(std::optional<std::filesystem::path> as) override;
    Result<void> open(const std::filesystem::path& path) override { return core_.openFile(path); }
    void newDocument() override { core_.newDocument(); }
    bool undo() override { return core_.undo(); }
    bool redo() override { return core_.redo(); }
    std::size_t messageCount() override { return core_.history().size(); }
    std::vector<std::string> messagesSince(std::size_t mark) override;
    std::optional<mcp::ViewInfo> view() override;
    void setView(const mcp::ViewInfo& v) override;
    void zoomToBox(const BBox2& box) override { core_.zoomToBox(box); }
    std::optional<mcp::Image> captureView(int maxPx, const BBox2& area) override;
    mcp::EditDecision requestEdit(const mcp::ClientInfo& client) override;
    void logToolCall(const mcp::ToolCallLog& log) override;

private:
    ui::AppCore& core_;
    mcp::OwnerQueue& queue_;
    Hooks hooks_;
};

}  // namespace tbcad::mcpapp
