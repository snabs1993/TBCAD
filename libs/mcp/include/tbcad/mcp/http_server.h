#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <functional>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "tbcad/mcp/protocol.h"

namespace tbcad::mcp {

struct HttpServerOptions {
    int port = 41730;     // if taken, any free port is used and reported by port()
    std::string token{};  // empty: 32 random bytes as hex
    std::size_t maxBodyBytes = 16 * 1024 * 1024;
    std::size_t maxSessions = 8;
    std::size_t maxConnections = 16;
    std::chrono::seconds sessionIdle{30 * 60};
};

struct HttpSessionInfo {
    std::string id{};
    std::string clientName{};
    std::string clientVersion{};
    std::chrono::system_clock::time_point started{};
    std::optional<EditDecision> decision{};
    bool streaming = false;
};

// MCP Streamable HTTP transport on 127.0.0.1 (never any other address): POST /mcp for requests
// and notifications, GET /mcp for the server-to-client event stream, DELETE /mcp to end a
// session. Every request needs `Authorization: Bearer <token>`; requests whose Origin or Host is
// not local are refused.
class HttpServer {
public:
    HttpServer(McpServerCore& core, HttpServerOptions options = {});
    ~HttpServer();
    HttpServer(const HttpServer&) = delete;
    HttpServer& operator=(const HttpServer&) = delete;

    Result<void> start();
    void stop();  // closes every connection and session; start() may be called again
    bool running() const { return running_; }

    int port() const { return port_; }
    const std::string& token() const { return options_.token; }
    std::string url() const { return "http://127.0.0.1:" + std::to_string(port_) + "/mcp"; }
    // Address the listener is bound to (always 127.0.0.1); for tests.
    const std::string& boundAddress() const { return boundAddress_; }

    std::vector<HttpSessionInfo> sessions() const;
    // Takes away the right to edit from one session (it keeps read access).
    bool revokeEdit(const std::string& sessionId);
    bool closeSession(const std::string& sessionId);
    void closeAllSessions();

    // Sends notifications/tools/list_changed to every open event stream.
    void notifyToolsChanged();
    // Called (from a server thread) whenever the list of sessions changes.
    std::function<void()> onSessionsChanged{};

private:
    struct Session;
    struct Connection;

    void acceptLoop();
    void serve(Connection* conn);
    bool handleRequest(Connection& conn, struct Request& req);
    void sweepSessions();
    std::shared_ptr<Session> findSession(const std::string& id);
    void changed();

    McpServerCore& core_;
    HttpServerOptions options_;
    std::atomic<bool> running_{false};
    std::atomic<bool> stopping_{false};
    std::intptr_t listener_ = -1;
    int port_ = 0;
    std::string boundAddress_{};
    std::thread acceptThread_{};

    mutable std::mutex mutex_{};
    std::map<std::string, std::shared_ptr<Session>> sessions_{};
    std::list<std::unique_ptr<Connection>> connections_;
};

// ---- Discovery file (how tbcad-mcp finds the running program) -----------------------------------

struct Discovery {
    std::string url{};
    std::string token{};
    long pid = 0;
    std::string version{};
};

// <data dir>/mcp.json
std::filesystem::path discoveryPath(const std::filesystem::path& dataDir);
Result<void> writeDiscovery(const std::filesystem::path& file, const Discovery& d);
std::optional<Discovery> readDiscovery(const std::filesystem::path& file);
void removeDiscovery(const std::filesystem::path& file);

// Hex of `bytes` random bytes.
std::string randomHex(std::size_t bytes);
// Id of this process (what the discovery file records).
long processId();

}  // namespace tbcad::mcp
