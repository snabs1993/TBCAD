#pragma once

#include <string>

#include "tbcad/mcp/json_rpc.h"

namespace tbcad::mcp {

// Checks `value` against the small JSON Schema subset the tools use: type (string or list of
// strings), required, properties, items, enum, minimum, maximum, minItems, maxItems, minLength,
// oneOf. Unknown keywords and unknown properties are ignored. Returns an empty string when the
// value is valid, else a readable message such as "arguments.ids[2]: expected string".
std::string validateSchema(const Json& schema, const Json& value, const std::string& path);

}  // namespace tbcad::mcp
