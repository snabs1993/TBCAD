#include "tbcad/mcp/http_client.h"

#include <algorithm>

#include "net/socket.h"

namespace tbcad::mcp {

namespace {

std::string lowerCase(std::string s) {
    for (char& c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string buildRequest(const std::string& method, const std::string& path, int port,
                         const std::map<std::string, std::string>& headers, const std::string& body,
                         bool hasBody) {
    std::string out = method + " " + path + " HTTP/1.1\r\n";
    out += "Host: 127.0.0.1:" + std::to_string(port) + "\r\n";
    out += "Connection: close\r\n";
    for (const auto& [k, v] : headers)
        out += k + ": " + v + "\r\n";
    if (hasBody)
        out += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    out += "\r\n";
    out += body;
    return out;
}

// Reads until the end of the headers. Returns the position after "\r\n\r\n" or npos.
bool readHead(net::Handle h, std::string& buf, std::size_t& headEnd, int timeoutMs) {
    char chunk[4096];
    for (;;) {
        std::size_t p = buf.find("\r\n\r\n");
        if (p != std::string::npos) {
            headEnd = p + 4;
            return true;
        }
        long n = net::receive(h, chunk, sizeof chunk, timeoutMs);
        if (n <= 0)
            return false;
        buf.append(chunk, static_cast<std::size_t>(n));
    }
}

bool parseHead(const std::string& buf, std::size_t headEnd, HttpResponse& res) {
    std::size_t eol = buf.find("\r\n");
    std::string line = buf.substr(0, eol);
    std::size_t sp = line.find(' ');
    if (sp == std::string::npos)
        return false;
    res.status = std::atoi(line.c_str() + sp + 1);
    std::size_t pos = eol + 2;
    while (pos < headEnd - 2) {
        std::size_t e = buf.find("\r\n", pos);
        std::string h = buf.substr(pos, e - pos);
        pos = e + 2;
        std::size_t colon = h.find(':');
        if (colon == std::string::npos)
            continue;
        std::string v = h.substr(colon + 1);
        std::size_t a = v.find_first_not_of(' ');
        res.headers[lowerCase(h.substr(0, colon))] = a == std::string::npos ? "" : v.substr(a);
    }
    return true;
}

}  // namespace

Result<HttpResponse> HttpClient::request(const std::string& method, const std::string& path,
                                         const std::map<std::string, std::string>& headers,
                                         const std::string& body, int timeoutMs) {
    net::Handle h = net::connectLoopback(port_, 3000);
    if (h == net::kInvalid)
        return makeError(ErrorCode::Io, "cannot connect to 127.0.0.1:" + std::to_string(port_));
    struct Closer {
        net::Handle h;
        ~Closer() { net::closeSocket(h); }
    } closer{h};
    bool hasBody = method == "POST" || !body.empty();
    std::string req = buildRequest(method, path, port_, headers, body, hasBody);
    if (!net::sendAll(h, req.data(), req.size()))
        return makeError(ErrorCode::Io, "send failed");
    std::string buf;
    std::size_t headEnd = 0;
    if (!readHead(h, buf, headEnd, timeoutMs))
        return makeError(ErrorCode::Io, "no response from the server");
    HttpResponse res;
    if (!parseHead(buf, headEnd, res))
        return makeError(ErrorCode::Parse, "bad response");
    std::size_t want = std::string::npos;
    if (auto it = res.headers.find("content-length"); it != res.headers.end())
        want = static_cast<std::size_t>(std::stoull(it->second));
    res.body = buf.substr(headEnd);
    char chunk[8192];
    while (want == std::string::npos || res.body.size() < want) {
        long n = net::receive(h, chunk, sizeof chunk, timeoutMs);
        if (n <= 0)
            break;
        res.body.append(chunk, static_cast<std::size_t>(n));
    }
    if (want != std::string::npos && res.body.size() > want)
        res.body.resize(want);
    return res;
}

Result<int> HttpClient::stream(const std::string& path,
                               const std::map<std::string, std::string>& headers,
                               const std::function<bool(const std::string&)>& onEvent,
                               const std::function<bool()>& keepGoing) {
    net::Handle h = net::connectLoopback(port_, 3000);
    if (h == net::kInvalid)
        return makeError(ErrorCode::Io, "cannot connect to 127.0.0.1:" + std::to_string(port_));
    struct Closer {
        net::Handle h;
        ~Closer() { net::closeSocket(h); }
    } closer{h};
    std::map<std::string, std::string> hd = headers;
    hd["Accept"] = "text/event-stream";
    std::string req = buildRequest("GET", path, port_, hd, "", false);
    if (!net::sendAll(h, req.data(), req.size()))
        return makeError(ErrorCode::Io, "send failed");
    std::string buf;
    std::size_t headEnd = 0;
    if (!readHead(h, buf, headEnd, 10000))
        return makeError(ErrorCode::Io, "no response from the server");
    HttpResponse res;
    if (!parseHead(buf, headEnd, res))
        return makeError(ErrorCode::Parse, "bad response");
    if (res.status != 200)
        return res.status;
    std::string pending = buf.substr(headEnd);
    char chunk[4096];
    for (;;) {
        // Complete events end with a blank line.
        std::size_t end;
        while ((end = pending.find("\n\n")) != std::string::npos) {
            std::string ev = pending.substr(0, end);
            pending.erase(0, end + 2);
            std::string data;
            std::size_t pos = 0;
            while (pos < ev.size()) {
                std::size_t e = ev.find('\n', pos);
                std::string ln =
                    ev.substr(pos, e == std::string::npos ? std::string::npos : e - pos);
                pos = e == std::string::npos ? ev.size() : e + 1;
                if (ln.rfind("data:", 0) == 0) {
                    if (!data.empty())
                        data += '\n';
                    std::size_t a = ln.find_first_not_of(' ', 5);
                    data += a == std::string::npos ? "" : ln.substr(a);
                }
            }
            if (!data.empty() && !onEvent(data))
                return 200;
        }
        if (!keepGoing())
            return 200;
        long n = net::receive(h, chunk, sizeof chunk, 500);
        if (n == net::kTimeout)
            continue;
        if (n <= 0)
            return 200;
        pending.append(chunk, static_cast<std::size_t>(n));
    }
}

}  // namespace tbcad::mcp
