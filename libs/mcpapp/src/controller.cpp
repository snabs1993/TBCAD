#include "tbcad/mcpapp/controller.h"

#include <algorithm>

#include "tbcad/mcp/tools.h"
#include "tbcad/platform/json.h"
#include "tbcad/platform/paths.h"

namespace tbcad::mcpapp {

namespace {

constexpr std::size_t kActivityLimit = 500;
constexpr auto kAskTimeout = std::chrono::seconds(120);

}  // namespace

McpController::McpController(ui::AppCore& core, Hooks hooks, std::string version)
    : core_(core),
      hooks_(std::move(hooks)),
      version_(std::move(version)),
      session_(core, queue_,
               AppSession::Hooks{
                   hooks_.capture,
                   [this](const mcp::ClientInfo& c) { return askUser(c); },
                   [this](const mcp::ToolCallLog& l) { onLog(l); },
               }) {
    queue_.setWake(hooks_.wake);
    mcp::ToolSet tools;
    mcp::registerDrawingTools(tools);
    for (const mcp::Tool& t : tools.all())
        editTools_[t.name] = t.edit;
    mcp::ServerInfo info;
    info.version = version_;
    mcpCore_ = std::make_unique<mcp::McpServerCore>(session_, std::move(tools), info);
    discoveryFile_ = mcp::discoveryPath(core_.paths().dataDir);
}

McpController::~McpController() {
    stop();
    queue_.close();
}

// ---- settings
// ------------------------------------------------------------------------------------

EditPolicy McpController::policy() const {
    const std::string v = core_.settings().getString(kSettingEditPermission, "ask");
    if (v == "allow")
        return EditPolicy::Allow;
    if (v == "readOnly")
        return EditPolicy::ReadOnly;
    return EditPolicy::Ask;
}

void McpController::setPolicy(EditPolicy p) {
    core_.settings().setString(kSettingEditPermission, p == EditPolicy::Allow      ? "allow"
                                                       : p == EditPolicy::ReadOnly ? "readOnly"
                                                                                   : "ask");
}

int McpController::configuredPort() const {
    const double p = core_.settings().getNumber(kSettingPort, 41730);
    return p >= 1 && p <= 65535 ? static_cast<int>(p) : 41730;
}

void McpController::setConfiguredPort(int port) {
    if (port >= 1 && port <= 65535)
        core_.settings().setNumber(kSettingPort, port);
}

// ---- start / stop
// --------------------------------------------------------------------------------

void McpController::start() {
    if (running_)
        return;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = false;
        lastError_.clear();
    }
    mcp::HttpServerOptions opt;
    opt.port = configuredPort();
    server_ = std::make_unique<mcp::HttpServer>(*mcpCore_, opt);
    server_->onSessionsChanged = [this] {
        if (hooks_.wake)
            hooks_.wake();
    };
    auto r = server_->start();
    if (!r) {
        std::lock_guard<std::mutex> lock(mutex_);
        lastError_ = r.error().message;
        server_.reset();
        return;
    }
    mcp::Discovery d;
    d.url = server_->url();
    d.token = server_->token();
    d.pid = mcp::processId();
    d.version = version_;
    auto w = mcp::writeDiscovery(discoveryFile_, d);
    running_ = true;
    if (!w) {
        std::lock_guard<std::mutex> lock(mutex_);
        lastError_ =
            "cannot write " + platform::pathToUtf8(discoveryFile_) + ": " + w.error().message;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        pendingHistory_.push_back("MCP server listening on " + d.url);
    }
}

void McpController::stop() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
    }
    cv_.notify_all();
    if (server_) {
        server_->stop();
        mcp::removeDiscovery(discoveryFile_);
        server_.reset();
    }
    if (running_.exchange(false)) {
        std::lock_guard<std::mutex> lock(mutex_);
        pendingHistory_.push_back("MCP server stopped");
    }
}

void McpController::setEnabled(bool on) {
    core_.settings().setBool(kSettingEnabled, on);
    if (on)
        start();
    else
        stop();
}

void McpController::pump() {
    const bool want = core_.settings().getBool(kSettingEnabled, false);
    if (want && !running_ && !server_) {
        // A failed start is not retried every frame: lastError_ stays until the user toggles.
        std::string err;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            err = lastError_;
        }
        if (err.empty())
            start();
    } else if (!want && running_) {
        stop();
    }
    queue_.pump(std::chrono::milliseconds(50));
    std::vector<std::string> lines;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        lines.swap(pendingHistory_);
    }
    for (const std::string& l : lines)
        core_.print(l);
}

// ---- permission
// ----------------------------------------------------------------------------------

mcp::EditDecision McpController::askUser(const mcp::ClientInfo& client) {
    EditPolicy p = EditPolicy::Ask;
    try {
        queue_.run([&] { p = policy(); });
    } catch (const std::exception&) {
        return mcp::EditDecision::Deny;
    }
    if (p == EditPolicy::Allow)
        return mcp::EditDecision::Allow;
    if (p == EditPolicy::ReadOnly)
        return mcp::EditDecision::ReadOnly;

    std::unique_lock<std::mutex> lock(mutex_);
    // One dialog at a time: wait for the previous request to be answered.
    cv_.wait(lock, [&] { return !request_ || stopping_; });
    if (stopping_)
        return mcp::EditDecision::Deny;
    request_ = EditRequest{nextRequestId_++, client};
    answer_.reset();
    lock.unlock();
    if (hooks_.wake)
        hooks_.wake();
    lock.lock();
    const bool got =
        cv_.wait_for(lock, kAskTimeout, [&] { return answer_.has_value() || stopping_; });
    mcp::EditDecision d = got && answer_ ? *answer_ : mcp::EditDecision::Deny;
    request_.reset();
    answer_.reset();
    lock.unlock();
    cv_.notify_all();
    return d;
}

std::optional<EditRequest> McpController::pendingRequest() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (request_ && !answer_)
        return request_;
    return std::nullopt;
}

void McpController::answerRequest(std::uint64_t id, mcp::EditDecision decision) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!request_ || request_->id != id || answer_)
            return;
        answer_ = decision;
    }
    cv_.notify_all();
}

// ---- activity
// ------------------------------------------------------------------------------------

void McpController::onLog(const mcp::ToolCallLog& log) {
    ActivityEntry e;
    e.time = std::chrono::system_clock::now();
    e.call = log;
    std::string line;
    auto it = editTools_.find(log.tool);
    if (it != editTools_.end() && it->second != mcp::Edit::None) {
        line = "[AI: " + log.client + "] " + log.tool;
        if (!log.summary.empty())
            line += ": " + log.summary;
        if (!log.ok)
            line += " (failed: " + log.error + ")";
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        activity_.push_back(std::move(e));
        while (activity_.size() > kActivityLimit)
            activity_.pop_front();
        if (!line.empty())
            pendingHistory_.push_back(std::move(line));
    }
    if (hooks_.wake)
        hooks_.wake();
}

std::vector<ActivityEntry> McpController::activity() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return std::vector<ActivityEntry>(activity_.begin(), activity_.end());
}

// ---- info
// ----------------------------------------------------------------------------------------

std::string McpController::lastError() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return lastError_;
}

std::string McpController::url() const {
    return server_ && running_ ? server_->url() : std::string{};
}

std::string McpController::token() const {
    return server_ && running_ ? server_->token() : std::string{};
}

std::vector<mcp::HttpSessionInfo> McpController::sessions() const {
    return server_ ? server_->sessions() : std::vector<mcp::HttpSessionInfo>{};
}

bool McpController::revokeEdit(const std::string& id) {
    return server_ && server_->revokeEdit(id);
}

bool McpController::closeSession(const std::string& id) {
    return server_ && server_->closeSession(id);
}

std::string McpController::claudeDesktopConfig() const {
    using platform::Json;
    Json cfg(Json::Object{
        {"mcpServers",
         Json(Json::Object{
             {"tbcad", Json(Json::Object{{"command", Json(platform::pathToUtf8(hooks_.bridgeExe))},
                                         {"args", Json(Json::Array{})}})}})}});
    return platform::toJson(cfg, true);
}

std::string McpController::claudeCodeCommand() const {
    return "claude mcp add tbcad -- \"" + platform::pathToUtf8(hooks_.bridgeExe) + "\"";
}

}  // namespace tbcad::mcpapp
