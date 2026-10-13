#include "tbcad/mcp/json_rpc.h"

#include <cmath>
#include <cstdio>

#include "tbcad/core/units.h"

namespace tbcad::mcp {

namespace {

void writeString(std::string& out, const std::string& s) {
    out += '"';
    for (char ch : s) {
        auto c = static_cast<unsigned char>(ch);
        switch (c) {
            case '"':
                out += "\\\"";
                break;
            case '\\':
                out += "\\\\";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof buf, "\\u%04x", c);
                    out += buf;
                } else {
                    out += ch;
                }
        }
    }
    out += '"';
}

void write(std::string& out, const Json& v) {
    if (v.isNull()) {
        out += "null";
    } else if (v.isBool()) {
        out += v.asBool() ? "true" : "false";
    } else if (v.isNumber()) {
        double d = v.asNumber();
        if (!std::isfinite(d)) {
            out += "null";
        } else {
            out += formatDouble(d);
        }
    } else if (v.isString()) {
        writeString(out, v.asString());
    } else if (v.isArray()) {
        out += '[';
        bool first = true;
        for (const auto& e : v.asArray()) {
            if (!first)
                out += ',';
            first = false;
            write(out, e);
        }
        out += ']';
    } else {
        out += '{';
        bool first = true;
        for (const auto& [k, e] : v.asObject()) {
            if (!first)
                out += ',';
            first = false;
            writeString(out, k);
            out += ':';
            write(out, e);
        }
        out += '}';
    }
}

Json errorOutcome(const Json& id, int code, const char* text) {
    return makeError(id, code, text);
}

}  // namespace

std::string dump(const Json& value) {
    std::string out;
    write(out, value);
    return out;
}

Json makeError(const Json& id, int code, std::string message) {
    Json::Object err;
    err["code"] = code;
    err["message"] = std::move(message);
    Json::Object o;
    o["jsonrpc"] = "2.0";
    o["id"] = id;
    o["error"] = Json(std::move(err));
    return Json(std::move(o));
}

Json makeResult(const Json& id, Json result) {
    Json::Object o;
    o["jsonrpc"] = "2.0";
    o["id"] = id;
    o["result"] = std::move(result);
    return Json(std::move(o));
}

Json makeNotification(std::string method, Json params) {
    Json::Object o;
    o["jsonrpc"] = "2.0";
    o["method"] = std::move(method);
    if (!params.isNull())
        o["params"] = std::move(params);
    return Json(std::move(o));
}

ParseOutcome parseMessage(std::string_view text) {
    auto parsed = platform::parseJson(text);
    if (!parsed)
        return {{}, errorOutcome(Json{}, rpc::kParseError, "Parse error")};
    return parseMessage(parsed.value());
}

ParseOutcome parseMessage(const Json& v) {
    if (v.isArray()) {
        return {{},
                errorOutcome(Json{}, rpc::kInvalidRequest,
                             "JSON-RPC batches are not supported (MCP 2025-06-18)")};
    }
    if (!v.isObject())
        return {{}, errorOutcome(Json{}, rpc::kInvalidRequest, "Invalid Request")};
    const Json* version = v.find("jsonrpc");
    const Json* idv = v.find("id");
    Json id = idv ? *idv : Json{};
    bool idOk = !idv || idv->isNumber() || idv->isString();
    if (!version || !version->isString() || version->asString() != "2.0" || !idOk) {
        return {{}, errorOutcome(idOk ? id : Json{}, rpc::kInvalidRequest, "Invalid Request")};
    }
    Message m;
    m.id = id;
    if (const Json* method = v.find("method")) {
        if (!method->isString())
            return {{}, errorOutcome(id, rpc::kInvalidRequest, "Invalid Request")};
        m.method = method->asString();
        m.kind = idv ? Message::Kind::Request : Message::Kind::Notification;
        if (const Json* p = v.find("params")) {
            if (!p->isObject() && !p->isArray() && !p->isNull())
                return {{}, errorOutcome(id, rpc::kInvalidRequest, "params must be structured")};
            m.params = *p;
        }
        return {std::move(m), {}};
    }
    if (idv && (v.find("result") || v.find("error"))) {
        m.kind = Message::Kind::Response;
        return {std::move(m), {}};
    }
    return {{}, errorOutcome(id, rpc::kInvalidRequest, "Invalid Request")};
}

}  // namespace tbcad::mcp
