#include "tbcad/mcpcli/bridge.h"

#include <chrono>
#include <ostream>

#include "tbcad/mcp/http_client.h"
#include "tbcad/mcp/tools.h"

namespace tbcad::mcpcli {

using mcp::Json;
using mcp::Message;

struct Bridge::Online {
    mcp::Discovery discovery{};
    int port = 0;
    std::string sessionId{};
    std::string protocol{};
};

namespace {

int portOf(const std::string& url) {
    // http://127.0.0.1:41730/mcp
    auto colon = url.rfind(':');
    if (colon == std::string::npos)
        return 0;
    return std::atoi(url.c_str() + colon + 1);
}

constexpr const char* kNotRunning =
    "TBCAD is not running or its MCP server is off. Open TBCAD and turn on Tools > MCP Server.";

Json errorToolResult(const std::string& text) {
    return Json(Json::Object{
        {"content", Json(Json::Array{Json(Json::Object{{"type", "text"}, {"text", text}})})},
        {"isError", true}});
}

}  // namespace

Bridge::Bridge(BridgeConfig config, std::ostream& out) : config_(std::move(config)), out_(out) {
    mcp::ToolSet tools;
    mcp::registerDrawingTools(tools);
    offlineSession_ = std::make_unique<mcp::HeadlessSession>(offlineRegistry_);
    offlineCore_ = std::make_unique<mcp::McpServerCore>(*offlineSession_, std::move(tools));
}

Bridge::~Bridge() {
    shutdown();
}

void Bridge::shutdown() {
    stopEventRelay();
    if (online_ && !online_->sessionId.empty()) {
        mcp::HttpClient http(online_->port);
        (void)http.request("DELETE", "/mcp",
                           {{"Authorization", "Bearer " + online_->discovery.token},
                            {"Mcp-Session-Id", online_->sessionId}},
                           "", 2000);
    }
    online_.reset();
}

void Bridge::write(const std::string& text) {
    std::lock_guard<std::mutex> lock(outMutex_);
    out_ << text << '\n';
    out_.flush();
}

void Bridge::answerOffline(const Message& m) {
    if (m.kind == Message::Kind::Notification) {
        if (m.method == "notifications/initialized")
            offlineClient_.ready = true;
        return;
    }
    if (m.kind != Message::Kind::Request)
        return;
    if (m.method == "tools/call") {
        write(mcp::dump(mcp::makeResult(m.id, errorToolResult(kNotRunning))));
        return;
    }
    if (m.method == "resources/read") {
        const Json* uri = m.params.find("uri");
        if (!uri || uri->asString() != "tbcad://docs/entity-json") {
            write(mcp::dump(mcp::makeError(m.id, -32002, kNotRunning)));
            return;
        }
    }
    auto reply = offlineCore_->handle(offlineClient_, m);
    if (reply)
        write(mcp::dump(*reply));
}

bool Bridge::connect(const mcp::Discovery& d) {
    int port = portOf(d.url);
    if (port <= 0 || !clientInitialized_)
        return false;
    mcp::HttpClient http(port);
    std::map<std::string, std::string> headers{{"Authorization", "Bearer " + d.token},
                                               {"Content-Type", "application/json"},
                                               {"Accept", "application/json, text/event-stream"}};
    Json::Object msg{
        {"jsonrpc", "2.0"}, {"id", 1}, {"method", "initialize"}, {"params", initParams_}};
    std::string body = mcp::dump(Json(std::move(msg)));
    auto r = http.request("POST", "/mcp", headers, body, 15000);
    if (!r || r.value().status != 200)
        return false;
    auto it = r.value().headers.find("mcp-session-id");
    if (it == r.value().headers.end())
        return false;
    auto parsed = platform::parseJson(r.value().body);
    auto online = std::make_unique<Online>();
    online->discovery = d;
    online->port = port;
    online->sessionId = it->second;
    if (parsed && parsed.value().find("result")) {
        if (const Json* pv = parsed.value().find("result")->find("protocolVersion"))
            online->protocol = pv->asString();
    }
    headers["Mcp-Session-Id"] = online->sessionId;
    (void)http.request("POST", "/mcp", headers,
                       mcp::dump(mcp::makeNotification("notifications/initialized")), 5000);
    stopEventRelay();
    online_ = std::move(online);
    startEventRelay();
    return true;
}

void Bridge::startEventRelay() {
    relayStop_ = false;
    int port = online_->port;
    std::map<std::string, std::string> headers{
        {"Authorization", "Bearer " + online_->discovery.token},
        {"Mcp-Session-Id", online_->sessionId}};
    relay_ = std::thread([this, port, headers] {
        mcp::HttpClient http(port);
        (void)http.stream(
            "/mcp", headers,
            [this](const std::string& data) {
                write(data);
                return true;
            },
            [this] { return !relayStop_.load(); });
    });
}

void Bridge::stopEventRelay() {
    relayStop_ = true;
    if (relay_.joinable())
        relay_.join();
}

bool Bridge::ensureOnline() {
    auto d = mcp::readDiscovery(config_.discoveryFile);
    if (!d && config_.launch && !launched_) {
        launched_ = true;
        if (config_.launch())
            d = mcp::readDiscovery(config_.discoveryFile);
    }
    if (!d) {
        online_.reset();
        return false;
    }
    if (online_ && online_->discovery.url == d->url && online_->discovery.token == d->token)
        return true;
    online_.reset();
    return connect(*d);
}

std::string Bridge::forward(const std::string& line, const Message& m, bool& retried) {
    for (;;) {
        if (!ensureOnline())
            return {};
        mcp::HttpClient http(online_->port);
        std::map<std::string, std::string> headers{
            {"Authorization", "Bearer " + online_->discovery.token},
            {"Content-Type", "application/json"},
            {"Accept", "application/json, text/event-stream"},
            {"Mcp-Session-Id", online_->sessionId}};
        if (!online_->protocol.empty())
            headers["MCP-Protocol-Version"] = online_->protocol;
        auto r = http.request("POST", "/mcp", headers, line, 200000);
        if (r && r.value().status == 404 && !retried) {
            // The session expired (or TBCAD restarted): start a new one and send again.
            retried = true;
            online_.reset();
            continue;
        }
        if (!r) {
            if (!retried) {
                retried = true;
                online_.reset();
                continue;
            }
            return m.kind == Message::Kind::Request
                       ? mcp::dump(mcp::makeResult(m.id, errorToolResult(kNotRunning)))
                       : std::string();
        }
        const auto& res = r.value();
        if (res.status == 200)
            return res.body;
        if (res.status == 202)
            return {};
        if (m.kind == Message::Kind::Request) {
            return mcp::dump(mcp::makeError(m.id, mcp::rpc::kInternalError,
                                            "TBCAD answered HTTP " + std::to_string(res.status)));
        }
        return {};
    }
}

void Bridge::handleLine(std::string_view line) {
    mcp::ParseOutcome p = mcp::parseMessage(line);
    if (p.error) {
        write(mcp::dump(*p.error));
        return;
    }
    const Message& m = *p.message;
    if (m.kind == Message::Kind::Response)
        return;
    if (m.kind == Message::Kind::Request && m.method == "initialize") {
        initParams_ = m.params;
        clientInitialized_ = true;
        online_.reset();
        // The client always gets the local handshake; the session with TBCAD is made on first
        // use (and again whenever TBCAD restarts). With --launch, start TBCAD right away.
        if (config_.launch)
            (void)ensureOnline();
        answerOffline(m);
        return;
    }
    if (m.method == "ping" || m.method == "notifications/initialized") {
        answerOffline(m);
        return;
    }
    if (!clientInitialized_) {
        answerOffline(m);  // reports "Server not initialized"
        return;
    }
    bool retried = false;
    std::string reply = forward(std::string(line), m, retried);
    if (!reply.empty()) {
        write(reply);
        return;
    }
    if (m.kind == Message::Kind::Request)
        answerOffline(m);
}

}  // namespace tbcad::mcpcli
