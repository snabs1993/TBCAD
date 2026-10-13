#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "tbcad/cmd/registry.h"
#include "tbcad/mcp/http_server.h"
#include "tbcad/mcp/owner_queue.h"
#include "tbcad/mcp/protocol.h"
#include "tbcad/mcpapp/app_session.h"
#include "tbcad/ui/app_core.h"
#include "tbcad/ui/ui_registry.h"

namespace tbcad::mcpapp {

// Settings keys (defaults are applied here, not in platform::Settings).
inline constexpr const char* kSettingEnabled = "mcp.enabled";                // bool, default off
inline constexpr const char* kSettingPort = "mcp.port";                      // default 41730
inline constexpr const char* kSettingEditPermission = "mcp.editPermission";  // ask|allow|readOnly

enum class EditPolicy { Ask, Allow, ReadOnly };

struct ActivityEntry {
    std::chrono::system_clock::time_point time{};
    mcp::ToolCallLog call{};
};

// A client waiting for the user to say whether it may edit.
struct EditRequest {
    std::uint64_t id = 0;
    mcp::ClientInfo client{};
};

// Owns the MCP server of a running program: the HTTP server, the discovery file, the activity log
// and the edit-permission state. Created on the UI thread after AppCore; call pump() once per
// frame.
class McpController {
public:
    struct Hooks {
        std::function<std::optional<mcp::Image>(int maxPx, const BBox2& area)> capture{};
        // Wakes the UI thread's event loop (glfwPostEmptyEvent); called from any thread.
        std::function<void()> wake{};
        // tbcad-mcp next to the program; shown in the "copy configuration" texts.
        std::filesystem::path bridgeExe{};
    };

    McpController(ui::AppCore& core, Hooks hooks, std::string version);
    ~McpController();
    McpController(const McpController&) = delete;
    McpController& operator=(const McpController&) = delete;

    // ---- UI thread ---------------------------------------------------------------------------
    // Once per frame: follows the mcp.enabled setting, runs queued tool work (at most ~50 ms) and
    // writes pending history lines.
    void pump();
    // Starts or stops the server now and remembers the choice in the settings.
    void setEnabled(bool on);
    void toggle() { setEnabled(!running()); }
    EditPolicy policy() const;
    void setPolicy(EditPolicy p);
    int configuredPort() const;
    void setConfiguredPort(int port);
    // "Tools > MCP Settings..." asks the window to show the panel.
    void requestSettings() { openSettings_ = true; }
    bool consumeSettingsRequest() { return openSettings_.exchange(false); }

    // Adds the Tools menu items, the status-bar toggle and the settings panel.
    void registerUi(ui::UiRegistry& ui);
    // Adds MCPTOGGLE and MCPSETTINGS to the command registry.
    void registerCommands(cmd::Registry& registry);
    // The confirm dialog (modal) for a client asking to edit. Call between NewFrame and Render.
    void drawDialogs();

    // ---- any thread --------------------------------------------------------------------------
    bool running() const { return running_; }
    std::string lastError() const;
    std::string url() const;
    std::string token() const;
    std::vector<mcp::HttpSessionInfo> sessions() const;
    bool revokeEdit(const std::string& sessionId);
    bool closeSession(const std::string& sessionId);
    std::vector<ActivityEntry> activity() const;
    std::optional<EditRequest> pendingRequest() const;
    // Answers the dialog. `forThisSession` is implied: the decision lasts for the MCP session.
    void answerRequest(std::uint64_t id, mcp::EditDecision decision);

    // For tests and the panel: the server and its core.
    mcp::HttpServer* server() { return server_.get(); }
    mcp::DrawingSession& session() { return session_; }
    // Texts for "Copy Claude Desktop config" and "Copy Claude Code command".
    std::string claudeDesktopConfig() const;
    std::string claudeCodeCommand() const;

private:
    void start();
    void stop();
    mcp::EditDecision askUser(const mcp::ClientInfo& client);
    void onLog(const mcp::ToolCallLog& log);
    void drawPanel(ui::PanelContext& ctx);

    ui::AppCore& core_;
    Hooks hooks_;
    std::string version_;
    mcp::OwnerQueue queue_;
    AppSession session_;
    std::unique_ptr<mcp::McpServerCore> mcpCore_;
    std::unique_ptr<mcp::HttpServer> server_;
    std::map<std::string, mcp::Edit> editTools_{};
    std::filesystem::path discoveryFile_{};
    std::atomic<bool> running_{false};
    std::atomic<bool> openSettings_{false};

    mutable std::mutex mutex_{};
    std::condition_variable cv_{};
    std::string lastError_{};
    std::deque<ActivityEntry> activity_{};
    std::vector<std::string> pendingHistory_{};
    std::optional<EditRequest> request_{};
    std::optional<mcp::EditDecision> answer_{};
    std::uint64_t nextRequestId_ = 1;
    bool stopping_ = false;
    std::string portText_{};  // panel input buffer
};

}  // namespace tbcad::mcpapp
