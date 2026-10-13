#include "tbcad/mcp/http_server.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <random>
#include <sstream>

#include "net/socket.h"
#include "tbcad/platform/paths.h"

namespace tbcad::mcp {

namespace {

constexpr std::size_t kMaxHeaderBytes = 64 * 1024;
constexpr int kPollMs = 500;             // how often blocked reads look at the stop flag
constexpr int kIdleKeepAliveMs = 60000;  // a quiet connection is closed after this
constexpr int kBodyTimeoutMs = 30000;
constexpr int kKeepaliveSeconds = 25;

std::string lowerCase(std::string s) {
    for (char& c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string trimmed(std::string_view s) {
    std::size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t'))
        ++a;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r'))
        --b;
    return std::string(s.substr(a, b - a));
}

bool constantTimeEquals(const std::string& a, const std::string& b) {
    unsigned diff = static_cast<unsigned>(a.size() ^ b.size());
    for (std::size_t i = 0; i < a.size() && i < b.size(); ++i)
        diff |= static_cast<unsigned>(static_cast<unsigned char>(a[i]) ^
                                      static_cast<unsigned char>(b[i]));
    return diff == 0;
}

// "localhost", "127.0.0.1" or "[::1]", optionally followed by ":port".
bool isLocalHost(std::string_view hostPort) {
    std::string h = lowerCase(std::string(hostPort));
    std::size_t colon = h.rfind(':');
    if (colon != std::string::npos && h.find(']') == std::string::npos) {
        std::string port = h.substr(colon + 1);
        if (port.empty() ||
            !std::all_of(port.begin(), port.end(), [](char c) { return c >= '0' && c <= '9'; }))
            return false;
        h = h.substr(0, colon);
    } else if (colon != std::string::npos && h.find(']') != std::string::npos &&
               colon > h.find(']')) {
        h = h.substr(0, colon);
    }
    return h == "localhost" || h == "127.0.0.1" || h == "[::1]";
}

bool isLocalOrigin(const std::string& origin) {
    std::string o = lowerCase(origin);
    for (const char* scheme : {"http://", "https://"}) {
        std::string s = scheme;
        if (o.rfind(s, 0) == 0)
            return isLocalHost(std::string_view(o).substr(s.size()));
    }
    return false;
}

const char* reasonPhrase(int status) {
    switch (status) {
        case 200:
            return "OK";
        case 202:
            return "Accepted";
        case 400:
            return "Bad Request";
        case 401:
            return "Unauthorized";
        case 403:
            return "Forbidden";
        case 404:
            return "Not Found";
        case 405:
            return "Method Not Allowed";
        case 406:
            return "Not Acceptable";
        case 409:
            return "Conflict";
        case 411:
            return "Length Required";
        case 413:
            return "Payload Too Large";
        case 503:
            return "Service Unavailable";
        default:
            return "Error";
    }
}

}  // namespace

struct Request {
    std::string method{};
    std::string path{};
    std::map<std::string, std::string> headers{};
    std::string body{};

    std::string header(const char* name) const {
        auto it = headers.find(name);
        return it == headers.end() ? std::string() : it->second;
    }
};

struct HttpServer::Session {
    std::string id{};
    Client client{};
    std::chrono::system_clock::time_point started{};
    std::atomic<std::int64_t> lastUsed{0};  // steady clock, seconds
    std::mutex mu{};
    std::condition_variable cv{};
    std::deque<std::string> outbox{};
    bool closed = false;
    bool streaming = false;

    void touch() {
        lastUsed = std::chrono::duration_cast<std::chrono::seconds>(
                       std::chrono::steady_clock::now().time_since_epoch())
                       .count();
    }
};

struct HttpServer::Connection {
    net::Handle handle = net::kInvalid;
    std::mutex mu{};
    std::thread thread{};
    std::atomic<bool> done{false};
};

long processId() {
    return net::currentProcessId();
}

std::string randomHex(std::size_t bytes) {
    std::random_device rd;
    static const char* digits = "0123456789abcdef";
    std::string out;
    out.reserve(bytes * 2);
    std::uint32_t word = 0;
    for (std::size_t i = 0; i < bytes; ++i) {
        if (i % 4 == 0)
            word = rd();
        auto b = static_cast<unsigned>(word & 0xFF);
        word >>= 8;
        out += digits[b >> 4];
        out += digits[b & 15];
    }
    return out;
}

HttpServer::HttpServer(McpServerCore& core, HttpServerOptions options)
    : core_(core), options_(std::move(options)) {}

HttpServer::~HttpServer() {
    stop();
}

Result<void> HttpServer::start() {
    if (running_)
        return {};
    if (!net::initialize())
        return makeError(ErrorCode::Io, "network start-up failed");
    std::string error;
    int bound = 0;
    net::Handle h = net::listenLoopback(options_.port, true, bound, error);
    if (h == net::kInvalid)
        return makeError(ErrorCode::Io, "cannot listen on 127.0.0.1: " + error);
    listener_ = h;
    port_ = bound;
    boundAddress_ = net::localAddress(h);
    if (options_.token.empty())
        options_.token = randomHex(32);
    stopping_ = false;
    running_ = true;
    core_.setToolsChangedSink([this] { notifyToolsChanged(); });
    acceptThread_ = std::thread([this] { acceptLoop(); });
    return {};
}

void HttpServer::stop() {
    if (!running_)
        return;
    stopping_ = true;
    if (acceptThread_.joinable())
        acceptThread_.join();
    net::closeSocket(listener_);
    listener_ = net::kInvalid;
    closeAllSessions();
    std::list<std::unique_ptr<Connection>> conns;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& c : connections_) {
            std::lock_guard<std::mutex> cl(c->mu);
            if (c->handle != net::kInvalid)
                net::shutdownSocket(c->handle);
        }
        conns = std::move(connections_);
        connections_.clear();
    }
    for (auto& c : conns) {
        if (c->thread.joinable())
            c->thread.join();
    }
    core_.setToolsChangedSink(nullptr);
    running_ = false;
}

void HttpServer::changed() {
    if (onSessionsChanged)
        onSessionsChanged();
}

std::shared_ptr<HttpServer::Session> HttpServer::findSession(const std::string& id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = sessions_.find(id);
    return it == sessions_.end() ? nullptr : it->second;
}

std::vector<HttpSessionInfo> HttpServer::sessions() const {
    std::vector<HttpSessionInfo> out;
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& [id, s] : sessions_) {
        HttpSessionInfo i;
        i.id = id;
        i.clientName = s->client.info.name;
        i.clientVersion = s->client.info.version;
        i.started = s->started;
        {
            std::lock_guard<std::mutex> cl(s->client.mutex);
            i.decision = s->client.decision;
        }
        {
            std::lock_guard<std::mutex> sl(s->mu);
            i.streaming = s->streaming;
        }
        out.push_back(std::move(i));
    }
    return out;
}

bool HttpServer::revokeEdit(const std::string& sessionId) {
    auto s = findSession(sessionId);
    if (!s)
        return false;
    {
        std::lock_guard<std::mutex> cl(s->client.mutex);
        s->client.decision = EditDecision::ReadOnly;
    }
    changed();
    return true;
}

bool HttpServer::closeSession(const std::string& sessionId) {
    std::shared_ptr<Session> s;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = sessions_.find(sessionId);
        if (it == sessions_.end())
            return false;
        s = it->second;
        sessions_.erase(it);
    }
    {
        std::lock_guard<std::mutex> sl(s->mu);
        s->closed = true;
    }
    s->cv.notify_all();
    changed();
    return true;
}

void HttpServer::closeAllSessions() {
    std::map<std::string, std::shared_ptr<Session>> all;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        all.swap(sessions_);
    }
    for (auto& [id, s] : all) {
        {
            std::lock_guard<std::mutex> sl(s->mu);
            s->closed = true;
        }
        s->cv.notify_all();
    }
    if (!all.empty())
        changed();
}

void HttpServer::notifyToolsChanged() {
    std::string text = dump(makeNotification("notifications/tools/list_changed"));
    std::vector<std::shared_ptr<Session>> all;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& [id, s] : sessions_)
            all.push_back(s);
    }
    for (auto& s : all) {
        {
            std::lock_guard<std::mutex> sl(s->mu);
            s->outbox.push_back(text);
        }
        s->cv.notify_all();
    }
}

void HttpServer::sweepSessions() {
    auto now = std::chrono::duration_cast<std::chrono::seconds>(
                   std::chrono::steady_clock::now().time_since_epoch())
                   .count();
    std::vector<std::string> expired;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& [id, s] : sessions_) {
            bool streaming;
            {
                std::lock_guard<std::mutex> sl(s->mu);
                streaming = s->streaming;
            }
            if (!streaming && now - s->lastUsed > options_.sessionIdle.count())
                expired.push_back(id);
        }
    }
    for (const auto& id : expired)
        closeSession(id);
}

void HttpServer::acceptLoop() {
    int sweepTicks = 0;
    while (!stopping_) {
        net::Handle h = net::acceptWithTimeout(listener_, 250);
        // Reap finished connections.
        {
            std::list<std::unique_ptr<Connection>> finished;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                for (auto it = connections_.begin(); it != connections_.end();) {
                    if ((*it)->done) {
                        finished.push_back(std::move(*it));
                        it = connections_.erase(it);
                    } else {
                        ++it;
                    }
                }
            }
            for (auto& c : finished) {
                if (c->thread.joinable())
                    c->thread.join();
            }
        }
        if (++sweepTicks >= 20) {
            sweepTicks = 0;
            sweepSessions();
        }
        if (h == net::kInvalid)
            continue;
        std::size_t open;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            open = connections_.size();
        }
        if (open >= options_.maxConnections) {
            static const char busy[] =
                "HTTP/1.1 503 Service Unavailable\r\nContent-Length: 0\r\nConnection: "
                "close\r\n\r\n";
            net::sendAll(h, busy, sizeof busy - 1);
            net::closeSocket(h);
            continue;
        }
        auto conn = std::make_unique<Connection>();
        conn->handle = h;
        Connection* raw = conn.get();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            connections_.push_back(std::move(conn));
        }
        raw->thread = std::thread([this, raw] {
            serve(raw);
            raw->done = true;
        });
    }
}

namespace {

enum class ReadResult { Ok, Closed, Bad, TooLarge, LengthRequired };

// Reads one request. `buf` keeps bytes that belong to the next one.
ReadResult readRequest(net::Handle h, std::string& buf, Request& req, std::size_t maxBody,
                       const std::atomic<bool>& stop, int idleMs) {
    char chunk[8192];
    std::size_t headEnd = std::string::npos;
    int waited = 0;
    for (;;) {
        headEnd = buf.find("\r\n\r\n");
        if (headEnd != std::string::npos)
            break;
        if (buf.size() > kMaxHeaderBytes)
            return ReadResult::Bad;
        if (stop)
            return ReadResult::Closed;
        long n = net::receive(h, chunk, sizeof chunk, kPollMs);
        if (n == net::kTimeout) {
            waited += kPollMs;
            if (waited >= idleMs)
                return ReadResult::Closed;
            continue;
        }
        if (n <= 0)
            return ReadResult::Closed;
        waited = 0;
        buf.append(chunk, static_cast<std::size_t>(n));
    }
    std::string head = buf.substr(0, headEnd);
    // Request line.
    std::size_t eol = head.find("\r\n");
    std::string line = head.substr(0, eol == std::string::npos ? head.size() : eol);
    std::size_t sp1 = line.find(' ');
    std::size_t sp2 = line.rfind(' ');
    if (sp1 == std::string::npos || sp2 == sp1)
        return ReadResult::Bad;
    req = Request{};
    req.method = line.substr(0, sp1);
    std::string target = line.substr(sp1 + 1, sp2 - sp1 - 1);
    std::size_t q = target.find('?');
    req.path = q == std::string::npos ? target : target.substr(0, q);
    // Headers.
    std::size_t pos = eol == std::string::npos ? head.size() : eol + 2;
    while (pos < head.size()) {
        std::size_t e = head.find("\r\n", pos);
        std::string h2 = head.substr(pos, e == std::string::npos ? std::string::npos : e - pos);
        pos = e == std::string::npos ? head.size() : e + 2;
        std::size_t colon = h2.find(':');
        if (colon == std::string::npos)
            return ReadResult::Bad;
        req.headers[lowerCase(h2.substr(0, colon))] =
            trimmed(std::string_view(h2).substr(colon + 1));
    }
    std::size_t consumed = headEnd + 4;
    if (req.headers.count("transfer-encoding"))
        return ReadResult::LengthRequired;  // chunked request bodies are not supported
    std::size_t length = 0;
    auto cl = req.headers.find("content-length");
    if (cl != req.headers.end()) {
        const std::string& v = cl->second;
        if (v.empty() || v.size() > 12 ||
            !std::all_of(v.begin(), v.end(), [](char c) { return c >= '0' && c <= '9'; }))
            return ReadResult::Bad;
        length = static_cast<std::size_t>(std::stoull(v));
        if (length > maxBody)
            return ReadResult::TooLarge;
    } else if (req.method == "POST") {
        return ReadResult::LengthRequired;
    }
    int bodyWaited = 0;
    while (buf.size() < consumed + length) {
        if (stop)
            return ReadResult::Closed;
        long n = net::receive(h, chunk, sizeof chunk, kPollMs);
        if (n == net::kTimeout) {
            bodyWaited += kPollMs;
            if (bodyWaited >= kBodyTimeoutMs)
                return ReadResult::Closed;
            continue;
        }
        if (n <= 0)
            return ReadResult::Closed;
        buf.append(chunk, static_cast<std::size_t>(n));
    }
    req.body = buf.substr(consumed, length);
    buf.erase(0, consumed + length);
    return ReadResult::Ok;
}

bool respond(net::Handle h, int status, const std::string& body, bool keepAlive,
             const std::vector<std::pair<std::string, std::string>>& extra = {},
             const char* contentType = "application/json") {
    std::string out = "HTTP/1.1 " + std::to_string(status) + " " + reasonPhrase(status) + "\r\n";
    out += "Content-Type: " + std::string(contentType) + "\r\n";
    out += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    out += "Cache-Control: no-store\r\n";
    out += keepAlive ? "Connection: keep-alive\r\n" : "Connection: close\r\n";
    for (const auto& [k, v] : extra)
        out += k + ": " + v + "\r\n";
    out += "\r\n";
    out += body;
    return net::sendAll(h, out.data(), out.size());
}

std::string errorBody(const std::string& message) {
    return dump(Json(Json::Object{{"error", message}}));
}

}  // namespace

void HttpServer::serve(Connection* conn) {
    std::string buf;
    for (;;) {
        Request req;
        net::Handle h = conn->handle;
        ReadResult rr =
            readRequest(h, buf, req, options_.maxBodyBytes, stopping_, kIdleKeepAliveMs);
        if (rr == ReadResult::Closed)
            break;
        if (rr == ReadResult::Bad) {
            respond(h, 400, errorBody("malformed request"), false);
            break;
        }
        if (rr == ReadResult::TooLarge) {
            respond(h, 413, errorBody("request body too large"), false);
            break;
        }
        if (rr == ReadResult::LengthRequired) {
            respond(h, 411,
                    errorBody("a Content-Length body is required (chunked is not supported)"),
                    false);
            break;
        }
        if (!handleRequest(*conn, req))
            break;
    }
    std::lock_guard<std::mutex> lock(conn->mu);
    if (conn->handle != net::kInvalid) {
        net::closeSocket(conn->handle);
        conn->handle = net::kInvalid;
    }
}

bool HttpServer::handleRequest(Connection& conn, Request& req) {
    net::Handle h = conn.handle;
    bool keep = lowerCase(req.header("connection")) != "close";
    auto reply = [&](int status, const std::string& body,
                     const std::vector<std::pair<std::string, std::string>>& extra = {}) {
        return respond(h, status, body, keep, extra) && keep;
    };

    // DNS-rebinding guard: a browser page cannot talk to this server.
    if (req.headers.count("origin") && !isLocalOrigin(req.header("origin")))
        return reply(403, errorBody("origin not allowed"));
    if (req.headers.count("host") && !isLocalHost(req.header("host")))
        return reply(403, errorBody("host not allowed"));

    std::string auth = req.header("authorization");
    static const std::string bearer = "Bearer ";
    if (auth.size() <= bearer.size() || lowerCase(auth.substr(0, bearer.size())) != "bearer " ||
        !constantTimeEquals(auth.substr(bearer.size()), options_.token)) {
        return reply(401, errorBody("missing or wrong bearer token"),
                     {{"WWW-Authenticate", "Bearer"}});
    }
    if (req.path != "/mcp")
        return reply(404, errorBody("not found"));
    if (req.method != "POST" && req.method != "GET" && req.method != "DELETE")
        return reply(405, errorBody("method not allowed"), {{"Allow", "POST, GET, DELETE"}});

    std::string pv = req.header("mcp-protocol-version");
    if (!pv.empty() && pv != kProtocolVersion && pv != kProtocolVersionOld)
        return reply(400, errorBody("unsupported MCP-Protocol-Version"));

    if (req.method == "POST") {
        ParseOutcome p = parseMessage(std::string_view(req.body));
        if (p.error)
            return reply(400, dump(*p.error));
        const Message& m = *p.message;
        if (m.kind == Message::Kind::Request && m.method == "initialize") {
            sweepSessions();
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (sessions_.size() >= options_.maxSessions)
                    return reply(503, errorBody("too many MCP sessions"));
            }
            auto s = std::make_shared<Session>();
            s->id = randomHex(16);
            s->started = std::chrono::system_clock::now();
            s->touch();
            auto result = core_.handle(s->client, m);
            if (!result)
                return reply(400, errorBody("initialize must be a request"));
            if (result->find("error"))
                return reply(200, dump(*result));
            {
                std::lock_guard<std::mutex> lock(mutex_);
                sessions_[s->id] = s;
            }
            changed();
            return reply(200, dump(*result), {{"Mcp-Session-Id", s->id}});
        }
        std::string sid = req.header("mcp-session-id");
        if (sid.empty())
            return reply(400, errorBody("missing Mcp-Session-Id header"));
        auto s = findSession(sid);
        if (!s)
            return reply(404, errorBody("unknown or expired session; initialize again"));
        s->touch();
        auto result = core_.handle(s->client, m);
        changed();  // client name or permission may have changed
        if (!result)
            return reply(202, "");
        return reply(200, dump(*result));
    }

    std::string sid = req.header("mcp-session-id");
    if (sid.empty())
        return reply(400, errorBody("missing Mcp-Session-Id header"));
    auto s = findSession(sid);
    if (!s)
        return reply(404, errorBody("unknown or expired session; initialize again"));
    s->touch();

    if (req.method == "DELETE") {
        closeSession(sid);
        return reply(200, "");
    }

    // GET: server-to-client event stream.
    if (req.header("accept").find("text/event-stream") == std::string::npos)
        return reply(406, errorBody("Accept: text/event-stream is required"));
    {
        std::lock_guard<std::mutex> sl(s->mu);
        if (s->streaming)
            return reply(409, errorBody("this session already has an event stream"));
        s->streaming = true;
    }
    changed();
    const std::string head =
        "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nCache-Control: no-cache\r\n"
        "Connection: close\r\n\r\n";
    bool ok = net::sendAll(h, head.data(), head.size());
    while (ok && !stopping_) {
        std::string next;
        bool closed = false;
        {
            std::unique_lock<std::mutex> sl(s->mu);
            s->cv.wait_for(sl, std::chrono::seconds(kKeepaliveSeconds),
                           [&] { return s->closed || !s->outbox.empty() || stopping_; });
            closed = s->closed;
            if (!s->outbox.empty()) {
                next = std::move(s->outbox.front());
                s->outbox.pop_front();
            }
        }
        if (closed || stopping_)
            break;
        std::string frame =
            next.empty() ? ": keepalive\n\n" : "event: message\ndata: " + next + "\n\n";
        ok = net::sendAll(h, frame.data(), frame.size());
    }
    {
        std::lock_guard<std::mutex> sl(s->mu);
        s->streaming = false;
    }
    changed();
    return false;
}

// ---- Discovery file
// -------------------------------------------------------------------------------

std::filesystem::path discoveryPath(const std::filesystem::path& dataDir) {
    return dataDir / "mcp.json";
}

Result<void> writeDiscovery(const std::filesystem::path& file, const Discovery& d) {
    std::error_code ec;
    if (file.has_parent_path())
        std::filesystem::create_directories(file.parent_path(), ec);
    auto tmp = file;
    tmp += ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out)
            return makeError(ErrorCode::Io, "cannot write " + tmp.string());
        out << dump(Json(Json::Object{{"url", d.url},
                                      {"token", d.token},
                                      {"pid", static_cast<double>(d.pid)},
                                      {"version", d.version}}))
            << '\n';
        out.flush();
        if (!out)
            return makeError(ErrorCode::Io, "cannot write " + tmp.string());
    }
    // The file holds the access token: owner only where the platform has such permissions.
    std::filesystem::permissions(
        tmp, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
        std::filesystem::perm_options::replace, ec);
    std::filesystem::rename(tmp, file, ec);
    if (ec) {
        std::filesystem::remove(tmp, ec);
        return makeError(ErrorCode::Io, "cannot replace " + file.string());
    }
    return {};
}

std::optional<Discovery> readDiscovery(const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in)
        return std::nullopt;
    std::ostringstream buf;
    buf << in.rdbuf();
    auto parsed = platform::parseJson(buf.str());
    if (!parsed || !parsed.value().isObject())
        return std::nullopt;
    const Json& j = parsed.value();
    Discovery d;
    if (const Json* v = j.find("url"))
        d.url = v->asString();
    if (const Json* v = j.find("token"))
        d.token = v->asString();
    if (const Json* v = j.find("pid"))
        d.pid = static_cast<long>(v->asNumber());
    if (const Json* v = j.find("version"))
        d.version = v->asString();
    if (d.url.empty() || d.token.empty())
        return std::nullopt;
    return d;
}

void removeDiscovery(const std::filesystem::path& file) {
    std::error_code ec;
    std::filesystem::remove(file, ec);
}

}  // namespace tbcad::mcp
