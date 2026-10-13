#pragma once

#include <atomic>
#include <filesystem>
#include <functional>
#include <iosfwd>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>

#include "tbcad/cmd/registry.h"
#include "tbcad/mcp/headless.h"
#include "tbcad/mcp/http_server.h"
#include "tbcad/mcp/protocol.h"

namespace tbcad::mcpcli {

struct BridgeConfig {
    std::filesystem::path discoveryFile{};
    // Starts TBCAD and waits for it to publish its discovery file. Null = never launch.
    std::function<bool()> launch{};
    std::ostream* log = nullptr;
};

// stdio <-> running TBCAD. Every JSON-RPC line from the AI client is forwarded to the HTTP
// server of the running program (found through the discovery file, looked up again on every
// message, so starting TBCAD later works without restarting the client). While TBCAD is not
// running the bridge answers initialize/ping/tools/list itself and makes tools/call fail with a
// clear message.
class Bridge {
public:
    Bridge(BridgeConfig config, std::ostream& out);
    ~Bridge();
    Bridge(const Bridge&) = delete;
    Bridge& operator=(const Bridge&) = delete;

    // Handles one line from the client and writes any reply to `out`.
    void handleLine(std::string_view line);
    void shutdown();

private:
    struct Online;

    void write(const std::string& text);
    void answerOffline(const mcp::Message& m);
    bool ensureOnline();
    bool connect(const mcp::Discovery& d);
    void startEventRelay();
    void stopEventRelay();
    std::string forward(const std::string& line, const mcp::Message& m, bool& retried);

    BridgeConfig config_;
    std::ostream& out_;
    std::mutex outMutex_{};

    // Offline answers come from a real protocol core over an empty drawing.
    cmd::Registry offlineRegistry_{};
    std::unique_ptr<mcp::HeadlessSession> offlineSession_{};
    std::unique_ptr<mcp::McpServerCore> offlineCore_{};
    mcp::Client offlineClient_{};

    mcp::Json initParams_{};  // what the client sent in initialize
    bool clientInitialized_ = false;
    bool launched_ = false;

    std::unique_ptr<Online> online_{};
    std::thread relay_{};
    std::atomic<bool> relayStop_{false};
};

}  // namespace tbcad::mcpcli
