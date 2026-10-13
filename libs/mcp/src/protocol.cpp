#include "tbcad/mcp/protocol.h"

#include <chrono>
#include <exception>

#include "tbcad/mcp/png.h"
#include "tbcad/mcp/schema.h"
#include "tools_common.h"

namespace tbcad::mcp {

namespace {

constexpr std::size_t kMaxTextBytes = 100 * 1024;

std::string summarizeArguments(const Json& args) {
    if (!args.isObject() || args.asObject().empty())
        return {};
    std::string out;
    for (const auto& [k, v] : args.asObject()) {
        if (!out.empty())
            out += ", ";
        out += k + "=";
        if (v.isArray())
            out += std::to_string(v.asArray().size()) + " items";
        else if (v.isObject())
            out += "{...}";
        else
            out += dump(v);
        if (out.size() > 120) {
            out.resize(117);
            out += "...";
            break;
        }
    }
    return out;
}

Json toolDescription(const Tool& t) {
    Json::Object o;
    o["name"] = t.name;
    o["title"] = t.title;
    o["description"] = t.description;
    o["inputSchema"] = t.inputSchema;
    if (!t.outputSchema.isNull())
        o["outputSchema"] = t.outputSchema;
    Json::Object ann;
    ann["title"] = t.title;
    ann["readOnlyHint"] = t.edit == Edit::None;
    if (t.edit != Edit::None) {
        ann["destructiveHint"] = t.destructive;
        ann["idempotentHint"] = t.idempotent;
    }
    ann["openWorldHint"] = false;
    o["annotations"] = Json(std::move(ann));
    return Json(std::move(o));
}

Json errorResult(const std::string& text) {
    return Json(Json::Object{
        {"content", Json(Json::Array{Json(Json::Object{{"type", "text"}, {"text", text}})})},
        {"isError", true}});
}

}  // namespace

McpServerCore::McpServerCore(DrawingSession& session, ToolSet tools, ServerInfo info)
    : session_(session), tools_(std::move(tools)), info_(std::move(info)) {}

Json McpServerCore::serverInstructions() const {
    return Json(
        "TBCAD 2D CAD. Units are millimetres, angles radians (or {\"deg\": n}); lengths may be "
        "{\"value\": 3, \"unit\": \"m\"}. Use list_layers and query_entities before editing. "
        "Each tool call that changes the drawing is one undo step (Ctrl+Z for the user). Prefer "
        "add_entities for geometry and run_command for anything else the command list offers "
        "(dimensions, hatch, trim ...). Check your work with capture_view when it is listed.");
}

std::optional<Json> McpServerCore::handle(Client& client, const Message& m) {
    if (m.kind == Message::Kind::Response)
        return std::nullopt;
    if (m.kind == Message::Kind::Notification) {
        if (m.method == "notifications/initialized")
            client.ready = true;
        return std::nullopt;
    }
    try {
        if (m.method == "initialize")
            return makeResult(m.id, initialize(client, m));
        if (m.method == "ping")
            return makeResult(m.id, Json(Json::Object{}));
        if (!client.initialized) {
            return makeError(m.id, rpc::kInvalidRequest,
                             "Server not initialized: send initialize first");
        }
        if (m.method == "tools/list")
            return makeResult(m.id, listTools());
        if (m.method == "tools/call") {
            const Json* name = m.params.find("name");
            if (!name || !name->isString())
                return makeError(m.id, rpc::kInvalidParams, "tools/call needs a tool name");
            const Json* args = m.params.find("arguments");
            Json empty{Json::Object{}};
            if (args && !args->isObject() && !args->isNull())
                return makeError(m.id, rpc::kInvalidParams, "arguments must be an object");
            return makeResult(
                m.id, callTool(client, name->asString(), args && args->isObject() ? *args : empty));
        }
        if (m.method == "resources/list")
            return makeResult(m.id, listResources());
        if (m.method == "resources/templates/list") {
            return makeResult(m.id, Json(Json::Object{{"resourceTemplates", Json(Json::Array{})}}));
        }
        if (m.method == "resources/read")
            return makeResult(m.id, readResource(m.params));
        if (m.method == "logging/setLevel") {
            if (const Json* l = m.params.find("level"); l && l->isString())
                client.logLevel = l->asString();
            return makeResult(m.id, Json(Json::Object{}));
        }
        return makeError(m.id, rpc::kMethodNotFound, "Method not found: " + m.method);
    } catch (const RpcError& e) {
        return makeError(m.id, e.code, e.message);
    } catch (const std::exception& e) {
        return makeError(m.id, rpc::kInternalError, std::string("Internal error: ") + e.what());
    }
}

std::string McpServerCore::handleText(Client& client, std::string_view text) {
    ParseOutcome p = parseMessage(text);
    if (p.error)
        return dump(*p.error);
    auto reply = handle(client, *p.message);
    return reply ? dump(*reply) : std::string();
}

Json McpServerCore::initialize(Client& client, const Message& m) {
    std::string requested;
    if (const Json* v = m.params.find("protocolVersion"); v && v->isString())
        requested = v->asString();
    client.protocol = (requested == kProtocolVersionOld) ? kProtocolVersionOld : kProtocolVersion;
    if (const Json* ci = m.params.find("clientInfo")) {
        if (const Json* n = ci->find("name"); n && n->isString() && !n->asString().empty())
            client.info.name = n->asString().substr(0, 80);
        if (const Json* v = ci->find("version"); v && v->isString())
            client.info.version = v->asString().substr(0, 40);
    }
    client.initialized = true;
    client.ready = false;
    client.decision.reset();
    Json::Object caps;
    caps["tools"] = Json(Json::Object{{"listChanged", true}});
    caps["resources"] = Json(Json::Object{{"subscribe", false}, {"listChanged", false}});
    caps["logging"] = Json(Json::Object{});
    return Json(Json::Object{{"protocolVersion", client.protocol},
                             {"capabilities", Json(std::move(caps))},
                             {"serverInfo", Json(Json::Object{{"name", info_.name},
                                                              {"title", info_.title},
                                                              {"version", info_.version}})},
                             {"instructions", serverInstructions()}});
}

Json McpServerCore::listTools() {
    // Tool availability depends on the session's registry and viewport, which the owner thread
    // may be changing; ask there.
    Json::Array list;
    session_.runOnOwnerThread([&] {
        for (const Tool* t : tools_.list(session_))
            list.push_back(toolDescription(*t));
    });
    return Json(Json::Object{{"tools", Json(std::move(list))}});
}

EditDecision McpServerCore::permission(Client& client) {
    std::lock_guard<std::mutex> lock(client.mutex);
    if (!client.decision)
        client.decision = session_.requestEdit(client.info);
    return *client.decision;
}

Json McpServerCore::callTool(Client& client, const std::string& name, const Json& arguments) {
    const Tool* tool = tools_.find(name);
    bool available = tool != nullptr;
    if (tool && tool->available) {
        session_.runOnOwnerThread([&] { available = tool->available(session_); });
    }
    if (!available)
        throw RpcError{rpc::kInvalidParams, "Unknown tool: " + name};

    auto started = std::chrono::steady_clock::now();
    ToolCallLog log;
    log.client = client.info.name;
    log.tool = name;
    log.summary = summarizeArguments(arguments);
    auto finish = [&](Json result, bool ok, std::string error) {
        log.ok = ok;
        log.error = std::move(error);
        log.milliseconds =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started)
                .count();
        session_.logToolCall(log);
        return result;
    };

    std::string schemaError = validateSchema(tool->inputSchema, arguments, "arguments");
    if (!schemaError.empty())
        return finish(errorResult("Invalid arguments: " + schemaError), false, schemaError);

    if (tool->edit != Edit::None) {
        if (permission(client) != EditDecision::Allow) {
            std::string msg =
                "Editing is not allowed for this session. The user can allow it in TBCAD "
                "(Tools > MCP Server).";
            return finish(errorResult(msg), false, msg);
        }
    }

    ToolResult result;
    std::string label = std::string(kAiLabelPrefix) + tool->name;
    session_.runOnOwnerThread([&] {
        cmd::UndoStack& undo = session_.undoStack();
        if (tool->edit != Edit::None && tool->needsIdle && session_.runner().active()) {
            const cmd::PromptInfo* p = session_.runner().prompt();
            result = ToolResult::fail("The user is running " +
                                      (p ? p->command : std::string("a command")) +
                                      " in TBCAD; try again when it finishes.");
            return;
        }
        ToolContext ctx{session_, client.info, info_.version};
        bool group = tool->edit == Edit::Document;
        std::size_t before = undo.undoCount();
        if (group)
            undo.beginGroup(label);
        try {
            result = tool->handler(ctx, arguments);
        } catch (const std::exception& e) {
            result = ToolResult::fail(std::string("Internal error: ") + e.what());
        }
        if (group) {
            undo.endGroup();
            // A failed call leaves nothing behind.
            if (result.isError && undo.undoCount() > before)
                (void)session_.undo();
        }
    });

    if (result.isError)
        return finish(errorResult(result.error), false, result.error);

    std::string text = dump(result.data);
    if (text.size() > kMaxTextBytes) {
        if (tool->edit == Edit::None) {
            std::string msg = "The result is too large (" + std::to_string(text.size()) +
                              " bytes). Narrow the query or use limit and cursor.";
            return finish(errorResult(msg), false, msg);
        }
        text.resize(kMaxTextBytes);
        text += "... (truncated)";
    }
    Json::Array content;
    content.push_back(Json(Json::Object{{"type", "text"}, {"text", text}}));
    if (result.image) {
        auto png = encodePng(result.image->rgba, result.image->width, result.image->height);
        if (!png.empty()) {
            content.push_back(Json(Json::Object{
                {"type", "image"}, {"mimeType", "image/png"}, {"data", base64Encode(png)}}));
        }
    }
    Json::Object out;
    out["content"] = Json(std::move(content));
    if (result.data.isObject())
        out["structuredContent"] = result.data;
    out["isError"] = false;
    return finish(Json(std::move(out)), true, {});
}

Json McpServerCore::listResources() {
    auto res = [](const char* uri, const char* name, const char* desc, const char* mime) {
        return Json(
            Json::Object{{"uri", uri}, {"name", name}, {"description", desc}, {"mimeType", mime}});
    };
    return Json(Json::Object{
        {"resources",
         Json(Json::Array{
             res("tbcad://drawing/info", "Drawing info", "Summary of the open drawing",
                 "application/json"),
             res("tbcad://drawing/layers", "Layers", "Layer tree with sub-layers",
                 "application/json"),
             res("tbcad://commands", "Commands", "TBCAD commands with aliases", "application/json"),
             res("tbcad://docs/entity-json", "Entity JSON format",
                 "How entities are written and read by the tools", "text/markdown")})}});
}

Json McpServerCore::readResource(const Json& params) {
    const Json* uriJ = params.find("uri");
    if (!uriJ || !uriJ->isString())
        throw RpcError{rpc::kInvalidParams, "resources/read needs a uri"};
    const std::string& uri = uriJ->asString();
    std::string text, mime = "application/json";
    if (uri == "tbcad://docs/entity-json") {
        text = entityJsonDoc();
        mime = "text/markdown";
    } else if (uri == "tbcad://drawing/info" || uri == "tbcad://drawing/layers" ||
               uri == "tbcad://commands") {
        session_.runOnOwnerThread([&] {
            if (uri == "tbcad://drawing/info")
                text = dump(detail::drawingInfoJson(session_, info_.version));
            else if (uri == "tbcad://drawing/layers")
                text = dump(detail::layersJson(session_.document(), true));
            else
                text = dump(detail::commandsJson(session_, ""));
        });
    } else {
        throw RpcError{-32002, "Resource not found: " + uri};
    }
    return Json(Json::Object{
        {"contents",
         Json(Json::Array{Json(Json::Object{{"uri", uri}, {"mimeType", mime}, {"text", text}})})}});
}

}  // namespace tbcad::mcp
