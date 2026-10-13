#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "tbcad/core/core.h"
#include "tbcad/mcp/json_rpc.h"

namespace tbcad::mcp {

// ---- Units ------------------------------------------------------------------------------------

// A length: a number (mm) or {"value": 3, "unit": "m"} (mm, cm, m, in, ft).
Result<double> lengthFromJson(const Json& j);
// An angle: a number (radians) or {"deg": 90}.
Result<double> angleFromJson(const Json& j);
// A point: [x, y] or {"x": 1, "y": 2}; every coordinate is a length.
Result<Vec2> pointFromJson(const Json& j);
Json pointToJson(Vec2 p);

// ---- Layers -----------------------------------------------------------------------------------

// Full path of a layer: "S-FLOOR~edge".
std::string layerPath(const Document& doc, LayerId id);
// Walks a path of names separated by '~'. Null when any step is missing.
const Layer* findLayerByPath(const Document& doc, std::string_view path);
// Like findLayerByPath but creates what is missing inside `tx`, which must be an open transaction
// of `doc` (new layers are top level or ByParent sub-layers). Fails when a name is invalid or the
// depth limit is hit. `created` (optional) receives the paths of the layers it made.
Result<LayerId> ensureLayerPath(const Document& doc, Transaction& tx, std::string_view path,
                                std::vector<std::string>* created = nullptr);

Json colorToJson(const Color& c);
Result<Color> colorFromJson(const Json& j, bool forLayer);
Json lineweightToJson(const LineWeight& w);
Result<LineWeight> lineweightFromJson(const Json& j, bool forLayer);
Json linetypeToJson(const Document& doc, LinetypeId id);
Result<LinetypeId> linetypeFromJson(const Document& doc, const Json& j, bool forLayer);

// ---- Entities ---------------------------------------------------------------------------------

std::string kindName(EntityKind kind);

// The documented JSON of one entity (docs/mcp.md).
Json entityToJson(const Document& doc, const Entity& e);

struct EntityParseOptions {
    // Layer used when the JSON has none (path); empty = the document's current layer.
    std::string defaultLayer{};
};

// Builds an entity from its JSON. The id (if any) is ignored. Kinds that cannot be written
// (dimension, leader, proxy) give an error that points at run_command.
Result<Entity> entityFromJson(const Document& doc, const Json& j, const EntityParseOptions& opt);

// Markdown description of the entity JSON, served as tbcad://docs/entity-json.
const std::string& entityJsonDoc();

}  // namespace tbcad::mcp
