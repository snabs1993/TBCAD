#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "tbcad/cmd/registry.h"
#include "tbcad/cmd/runner.h"
#include "tbcad/cmd/undo.h"
#include "tbcad/core/core.h"
#include "tbcad/index/spatial_index.h"

namespace tbcad::mcp {

struct ClientInfo {
    std::string name = "unknown client";
    std::string version{};
    std::string sessionId{};
};

enum class EditDecision { Allow, ReadOnly, Deny };

struct FileInfo {
    std::string name = "Untitled";
    std::filesystem::path path{};
    bool hasFile = false;
    bool modified = false;
};

struct ViewInfo {
    Vec2 center{};
    double mmPerPixel = 1;
    int widthPx = 0;
    int heightPx = 0;
};

// 8-bit RGBA, rows top to bottom.
struct Image {
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> rgba{};
    BBox2 worldBox{};         // what the picture shows
    double mmPerPixel = 0;
};

// One finished tool call, for the activity panel and the command history.
struct ToolCallLog {
    std::string client{};
    std::string tool{};
    std::string summary{};  // one line of arguments
    std::string error{};    // empty when the call succeeded
    double milliseconds = 0;
    bool ok = true;
};

// The seam between the protocol/tools and the program. HeadlessSession (libs/mcp) and AppSession
// (libs/mcpapp) implement it; the tools are written once against this interface.
class DrawingSession {
public:
    virtual ~DrawingSession() = default;

    // Runs fn on the thread that owns the document and waits for it. Headless: runs inline.
    // An exception thrown by fn is rethrown in the caller.
    virtual void runOnOwnerThread(std::function<void()> fn) = 0;

    virtual Document& document() = 0;
    virtual cmd::UndoStack& undoStack() = 0;
    virtual cmd::Registry& registry() = 0;
    virtual cmd::CommandRunner& runner() = 0;
    // Null = no spatial index, queries scan the document.
    virtual const index::SpatialIndex* spatialIndex() { return nullptr; }

    virtual std::vector<EntityId> selection() = 0;
    virtual void setSelection(std::vector<EntityId> ids) = 0;

    virtual FileInfo file() = 0;
    // `as` empty = save to the current file.
    virtual Result<void> save(std::optional<std::filesystem::path> as) = 0;
    virtual Result<void> open(const std::filesystem::path& path) = 0;
    virtual void newDocument() = 0;

    // Undo and redo one step the way the program does (selection and scene follow).
    virtual bool undo() { return runner().undo(); }
    virtual bool redo() { return runner().redo(); }

    // Messages printed by commands so far (the command history); lets run_command report what a
    // command said.
    virtual std::size_t messageCount() = 0;
    virtual std::vector<std::string> messagesSince(std::size_t mark) = 0;

    // Viewport. Headless sessions have none.
    virtual std::optional<ViewInfo> view() { return std::nullopt; }
    virtual void setView(const ViewInfo&) {}
    virtual void zoomToBox(const BBox2&) {}
    // RGBA picture of `area` (world box; empty = what the viewport shows now), scaled so its
    // longer side is at most maxPx. The user's view is not changed. nullopt = unsupported or
    // failed.
    virtual std::optional<Image> captureView(int /*maxPx*/, const BBox2& /*area*/) {
        return std::nullopt;
    }

    // Edit permission for one MCP session; may block (dialog). Called on the thread that serves
    // the request, never on the owner thread.
    virtual EditDecision requestEdit(const ClientInfo& client) = 0;

    virtual void logToolCall(const ToolCallLog&) {}
};

}  // namespace tbcad::mcp
