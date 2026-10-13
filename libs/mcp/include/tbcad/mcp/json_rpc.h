#pragma once

#include <optional>
#include <string>
#include <string_view>

#include "tbcad/core/result.h"
#include "tbcad/platform/json.h"

namespace tbcad::mcp {

using Json = platform::Json;

// JSON-RPC 2.0 error codes.
namespace rpc {
inline constexpr int kParseError = -32700;
inline constexpr int kInvalidRequest = -32600;
inline constexpr int kMethodNotFound = -32601;
inline constexpr int kInvalidParams = -32602;
inline constexpr int kInternalError = -32603;
}  // namespace rpc

// Compact one-line JSON with the shortest round-trip number format. Never contains a raw newline.
std::string dump(const Json& value);

// Builds {"jsonrpc":"2.0","id":...,"error":{"code":..,"message":..}}. `id` may be null.
Json makeError(const Json& id, int code, std::string message);
Json makeResult(const Json& id, Json result);
Json makeNotification(std::string method, Json params = {});

// A parsed incoming message. A request has an id, a notification does not; a response (from the
// client) has `result` or `error` and no method.
struct Message {
    enum class Kind { Request, Notification, Response };
    Kind kind = Kind::Request;
    Json id{};
    std::string method{};
    Json params{};
};

struct ParseOutcome {
    std::optional<Message> message{};
    // Set when the text cannot be handled; send it back as the reply.
    std::optional<Json> error{};
};

// Parses one JSON-RPC message (batches are refused with -32600).
ParseOutcome parseMessage(std::string_view text);
ParseOutcome parseMessage(const Json& value);

}  // namespace tbcad::mcp
