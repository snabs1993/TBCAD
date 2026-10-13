#pragma once

#include <doctest/doctest.h>

#include <memory>
#include <set>

#include "tbcad/cmd/cmd.h"
#include "tbcad/core/dump.h"
#include "tbcad/mcp/entity_json.h"
#include "tbcad/mcp/headless.h"
#include "tbcad/mcp/protocol.h"
#include "tbcad/mcp/tools.h"

namespace tbcad::mcp::test {

// TESTLINE: two points make a line (stands in for a real drawing command).
class TestLine final : public cmd::Command {
public:
    std::string_view name() const override { return "TESTLINE"; }
    std::vector<std::string> defaultAliases() const override { return {"TL"}; }
    std::string_view description() const override { return "test line"; }
    cmd::Task run(cmd::CommandContext& ctx) override {
        auto a = co_await ctx.getPoint("First point");
        if (!a.ok())
            co_return;
        cmd::PointOptions o;
        o.base = a.point;
        auto b = co_await ctx.getPoint("Second point", o);
        if (!b.ok())
            co_return;
        (void)ctx.tx().add(Entity{{}, LineData{a.point, b.point}});
        ctx.message("line made");
    }
};

class HiddenCmd final : public cmd::Command {
public:
    std::string_view name() const override { return "DEVGEN"; }
    cmd::Task run(cmd::CommandContext&) override { co_return; }
};

inline Json J(const std::string& text) {
    auto p = platform::parseJson(text);
    REQUIRE_MESSAGE(p.ok(), text);
    return p.value();
}

struct Rig {
    cmd::Registry reg{};
    HeadlessSession session;
    McpServerCore core;
    Client client{};
    int nextId = 1;

    Rig() : session(prepare(reg)), core(session, makeTools()) {}

    static cmd::Registry& prepare(cmd::Registry& r) {
        cmd::registerBuiltinCommands(r);
        (void)r.add(std::make_unique<TestLine>());
        (void)r.add(std::make_unique<HiddenCmd>());
        return r;
    }
    static ToolSet makeTools() {
        ToolSet t;
        registerDrawingTools(t);
        return t;
    }

    // Sends a request and returns the whole reply object.
    Json rpc(const std::string& method, Json params = Json(Json::Object{})) {
        Json::Object m;
        m["jsonrpc"] = "2.0";
        m["id"] = nextId++;
        m["method"] = method;
        m["params"] = std::move(params);
        ParseOutcome p = parseMessage(Json(std::move(m)));
        REQUIRE(p.message.has_value());
        auto reply = core.handle(client, *p.message);
        REQUIRE(reply.has_value());
        return *reply;
    }

    void initialize(const std::string& version = "2025-06-18") {
        Json r = rpc("initialize",
                     J("{\"protocolVersion\":\"" + version +
                       "\",\"capabilities\":{},\"clientInfo\":{\"name\":\"test-client\","
                       "\"version\":\"1\"}}"));
        REQUIRE(r.find("result"));
        Message n;
        n.kind = Message::Kind::Notification;
        n.method = "notifications/initialized";
        core.handle(client, n);
    }

    // tools/call -> the CallToolResult object.
    Json call(const std::string& name, Json args = Json(Json::Object{})) {
        Json r = rpc("tools/call", Json(Json::Object{{"name", name}, {"arguments", std::move(args)}}));
        const Json* res = r.find("result");
        REQUIRE_MESSAGE(res, dump(r));
        return *res;
    }
    // The structured data of a successful call; fails the test on an error result.
    Json ok(const std::string& name, Json args = Json(Json::Object{})) {
        Json res = call(name, std::move(args));
        INFO(dump(res));
        REQUIRE_FALSE(res.find("isError")->asBool());
        const Json* s = res.find("structuredContent");
        REQUIRE(s);
        return *s;
    }
    // The error text of a call that must fail.
    std::string fails(const std::string& name, Json args = Json(Json::Object{})) {
        Json res = call(name, std::move(args));
        INFO(dump(res));
        REQUIRE(res.find("isError")->asBool());
        return res.find("content")->asArray()[0].find("text")->asString();
    }

    std::string dumpDoc() { return canonicalDump(session.document()); }
};

}  // namespace tbcad::mcp::test
