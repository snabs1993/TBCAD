#include <atomic>
#include <cstring>
#include <thread>

#include "net/socket.h"
#include "rig.h"
#include "tbcad/mcp/http_client.h"
#include "tbcad/mcp/http_server.h"
#include "tbcad/mcp/owner_queue.h"

using namespace tbcad;
using namespace tbcad::mcp;
using namespace tbcad::mcp::test;

namespace {

struct HttpRig {
    Rig rig;
    std::unique_ptr<HttpServer> server;
    std::unique_ptr<HttpClient> client;

    explicit HttpRig(HttpServerOptions opt = {}) {
        opt.port = 0;
        server = std::make_unique<HttpServer>(rig.core, opt);
        auto r = server->start();
        if (!r.ok())
            FAIL(r.error().message);
        client = std::make_unique<HttpClient>(server->port());
    }

    std::map<std::string, std::string> auth(std::map<std::string, std::string> extra = {}) {
        extra["Authorization"] = "Bearer " + server->token();
        extra["Content-Type"] = "application/json";
        return extra;
    }

    HttpResponse post(const std::string& body, std::map<std::string, std::string> headers) {
        auto r = client->request("POST", "/mcp", headers, body);
        REQUIRE(r.ok());
        return r.value();
    }

    // Initializes and returns the session id.
    std::string initialize() {
        HttpResponse r = post(
            R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-06-18","clientInfo":{"name":"http-test","version":"1"}}})",
            auth());
        REQUIRE(r.status == 200);
        REQUIRE(r.headers.count("mcp-session-id"));
        return r.headers["mcp-session-id"];
    }
};

}  // namespace

TEST_CASE("http: authentication and origin checks") {
    HttpRig h;
    const std::string ping = R"({"jsonrpc":"2.0","id":1,"method":"ping"})";
    CHECK(h.post(ping, {{"Content-Type", "application/json"}}).status == 401);
    CHECK(h.post(ping, {{"Authorization", "Bearer nope"}}).status == 401);
    CHECK(h.post(ping, {{"Authorization", "Basic " + h.server->token()}}).status == 401);
    CHECK(h.post(ping, h.auth({{"Origin", "https://evil.example"}})).status == 403);
    CHECK(h.post(ping, h.auth({{"Origin", "http://localhost.evil.example"}})).status == 403);
    CHECK(h.post(ping, h.auth({{"Origin", "null"}})).status == 403);
    // Local origins pass the origin check (then fail for the missing session).
    CHECK(h.post(ping, h.auth({{"Origin", "http://localhost:3000"}})).status == 400);
    CHECK(h.post(ping, h.auth({{"Origin", "http://127.0.0.1"}})).status == 400);
    CHECK(h.post(ping, h.auth({{"Host", "attacker.example"}})).status == 403);
    // Wrong path and method.
    auto r = h.client->request("POST", "/other", h.auth(), ping);
    REQUIRE(r.ok());
    CHECK(r.value().status == 404);
    r = h.client->request("PUT", "/mcp", h.auth(), ping);
    REQUIRE(r.ok());
    CHECK(r.value().status == 405);
}

TEST_CASE("http: the server listens on loopback only") {
    HttpRig h;
    CHECK(h.server->boundAddress() == "127.0.0.1");
    CHECK(h.server->port() > 0);
    CHECK(h.server->token().size() == 64);
}

TEST_CASE("http: session lifecycle") {
    HttpRig h;
    const std::string list = R"({"jsonrpc":"2.0","id":2,"method":"tools/list"})";
    std::string sid = h.initialize();
    CHECK(sid.size() == 32);
    CHECK(h.server->sessions().size() == 1);
    CHECK(h.server->sessions()[0].clientName == "http-test");

    // Missing and unknown session ids.
    CHECK(h.post(list, h.auth()).status == 400);
    CHECK(h.post(list, h.auth({{"Mcp-Session-Id", "deadbeef"}})).status == 404);
    // Bad protocol version header.
    CHECK(h.post(list, h.auth({{"Mcp-Session-Id", sid}, {"MCP-Protocol-Version", "1999-01-01"}}))
              .status == 400);

    HttpResponse ok =
        h.post(list, h.auth({{"Mcp-Session-Id", sid}, {"MCP-Protocol-Version", "2025-06-18"}}));
    REQUIRE(ok.status == 200);
    CHECK(J(ok.body).find("result")->find("tools")->asArray().size() > 10);

    // A notification is accepted without a body.
    HttpResponse n = h.post(R"({"jsonrpc":"2.0","method":"notifications/initialized"})",
                            h.auth({{"Mcp-Session-Id", sid}}));
    CHECK(n.status == 202);
    CHECK(n.body.empty());

    // Garbage body.
    HttpResponse bad = h.post("{oops", h.auth({{"Mcp-Session-Id", sid}}));
    CHECK(bad.status == 400);
    CHECK(J(bad.body).find("error")->find("code")->asNumber() == rpc::kParseError);

    // DELETE ends the session.
    auto del = h.client->request("DELETE", "/mcp", h.auth({{"Mcp-Session-Id", sid}}), "");
    REQUIRE(del.ok());
    CHECK(del.value().status == 200);
    CHECK(h.server->sessions().empty());
    CHECK(h.post(list, h.auth({{"Mcp-Session-Id", sid}})).status == 404);
}

TEST_CASE("http: a drawing round trip over the wire") {
    HttpRig h;
    std::string sid = h.initialize();
    auto call = [&](const std::string& name, const std::string& args) {
        HttpResponse r =
            h.post(R"({"jsonrpc":"2.0","id":9,"method":"tools/call","params":{"name":")" + name +
                       R"(","arguments":)" + args + "}}",
                   h.auth({{"Mcp-Session-Id", sid}}));
        REQUIRE(r.status == 200);
        return J(r.body);
    };
    Json r = call("add_entities", R"({"entities":[{"kind":"line","start":[0,0],"end":[10,0]}]})");
    CHECK_FALSE(r.find("result")->find("isError")->asBool());
    CHECK(h.rig.session.document().modelSpaceCount() == 1);
    r = call("get_drawing_info", "{}");
    CHECK(r.find("result")->find("structuredContent")->find("entityCount")->asNumber() == 1);
}

TEST_CASE("http: body and session limits") {
    HttpServerOptions opt;
    opt.maxBodyBytes = 1024;
    opt.maxSessions = 2;
    HttpRig h(opt);
    // Too large: a real body and a lying Content-Length.
    CHECK(h.post(std::string(2048, 'x'), h.auth()).status == 413);
    net::Handle s = net::connectLoopback(h.server->port(), 2000);
    REQUIRE(s != net::kInvalid);
    std::string raw = "POST /mcp HTTP/1.1\r\nAuthorization: Bearer " + h.server->token() +
                      "\r\nContent-Length: 20000000\r\n\r\n";
    net::sendAll(s, raw.data(), raw.size());
    char buf[256] = {};
    long n = net::receive(s, buf, sizeof buf - 1, 3000);
    REQUIRE(n > 0);
    CHECK(std::string(buf).find("413") != std::string::npos);
    net::closeSocket(s);

    // Chunked request bodies are refused.
    s = net::connectLoopback(h.server->port(), 2000);
    raw = "POST /mcp HTTP/1.1\r\nAuthorization: Bearer " + h.server->token() +
          "\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n\r\n";
    net::sendAll(s, raw.data(), raw.size());
    std::memset(buf, 0, sizeof buf);
    n = net::receive(s, buf, sizeof buf - 1, 3000);
    REQUIRE(n > 0);
    CHECK(std::string(buf).find("411") != std::string::npos);
    net::closeSocket(s);

    h.initialize();
    h.initialize();
    HttpResponse r =
        h.post(R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{}})", h.auth());
    CHECK(r.status == 503);
}

TEST_CASE("http: event stream delivers tools/list_changed") {
    HttpRig h;
    std::string sid = h.initialize();
    std::atomic<bool> got{false};
    std::atomic<bool> stop{false};
    std::string data;
    std::thread t([&] {
        (void)h.client->stream(
            "/mcp", h.auth({{"Mcp-Session-Id", sid}}),
            [&](const std::string& d) {
                data = d;
                got = true;
                return false;
            },
            [&] { return !stop.load(); });
    });
    // Wait until the stream is open, then notify.
    for (int i = 0;
         i < 100 && !(h.server->sessions().size() == 1 && h.server->sessions()[0].streaming); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    h.server->notifyToolsChanged();
    for (int i = 0; i < 100 && !got; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    stop = true;
    t.join();
    REQUIRE(got);
    CHECK(J(data).find("method")->asString() == "notifications/tools/list_changed");

    // A stream needs the right Accept header.
    auto r = h.client->request("GET", "/mcp", h.auth({{"Mcp-Session-Id", sid}}), "");
    REQUIRE(r.ok());
    CHECK(r.value().status == 406);
}

TEST_CASE("http: stopping closes sessions and the port") {
    HttpRig h;
    h.initialize();
    int port = h.server->port();
    h.server->stop();
    CHECK_FALSE(h.server->running());
    CHECK(h.server->sessions().empty());
    CHECK(net::connectLoopback(port, 300) == net::kInvalid);
}

TEST_CASE("discovery file round trip") {
    auto dir = std::filesystem::temp_directory_path() / "tbcad_mcp_disc";
    std::filesystem::remove_all(dir);
    auto file = discoveryPath(dir);
    CHECK_FALSE(readDiscovery(file).has_value());
    Discovery d{"http://127.0.0.1:41730/mcp", "abc123", 4242, "0.1"};
    REQUIRE(writeDiscovery(file, d).ok());
    auto back = readDiscovery(file);
    REQUIRE(back);
    CHECK(back->url == d.url);
    CHECK(back->token == "abc123");
    CHECK(back->pid == 4242);
    removeDiscovery(file);
    CHECK_FALSE(readDiscovery(file).has_value());
    std::filesystem::remove_all(dir);
}

TEST_CASE("owner queue: work runs only inside pump") {
    OwnerQueue q;
    std::atomic<int> ran{0};
    std::atomic<bool> finished{false};
    std::thread t([&] {
        q.run([&] { ++ran; });
        finished = true;
    });
    for (int i = 0; i < 100 && !q.hasPending(); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    REQUIRE(q.hasPending());
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    CHECK(ran == 0);
    CHECK_FALSE(finished);
    CHECK(q.pump() == 1);
    t.join();
    CHECK(ran == 1);
    CHECK(finished);
}

TEST_CASE(
    "owner queue: the time budget limits a frame, exceptions travel back, close fails waiters") {
    OwnerQueue q;
    std::atomic<int> wakes{0};
    q.setWake([&] { ++wakes; });
    std::atomic<int> ran{0};
    std::vector<std::thread> threads;
    for (int i = 0; i < 3; ++i) {
        threads.emplace_back([&] {
            q.run([&] {
                std::this_thread::sleep_for(std::chrono::milliseconds(30));
                ++ran;
            });
        });
    }
    for (int i = 0; i < 200 && wakes < 3; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    REQUIRE(wakes >= 3);
    std::size_t first = q.pump(std::chrono::milliseconds(10));
    CHECK(first >= 1);
    CHECK(first < 3);  // the budget cut the frame short
    CHECK(q.hasPending());
    while (q.hasPending())
        q.pump(std::chrono::milliseconds(1000));
    for (auto& t : threads)
        t.join();
    CHECK(ran == 3);

    std::string what;
    std::thread thrower([&] {
        try {
            q.run([] { throw std::runtime_error("boom"); });
        } catch (const std::exception& e) {
            what = e.what();
        }
    });
    for (int i = 0; i < 100 && !q.hasPending(); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    q.pump();
    thrower.join();
    CHECK(what == "boom");

    std::string closedWhat;
    std::thread waiter([&] {
        try {
            q.run([] {});
        } catch (const std::exception& e) {
            closedWhat = e.what();
        }
    });
    for (int i = 0; i < 100 && !q.hasPending(); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    q.close();
    waiter.join();
    CHECK(closedWhat.find("shutting down") != std::string::npos);
    bool lateThrew = false;
    std::thread late([&] {
        try {
            q.run([] {});
        } catch (const std::exception&) {
            lateThrew = true;
        }
    });
    late.join();
    CHECK(lateThrew);
}
