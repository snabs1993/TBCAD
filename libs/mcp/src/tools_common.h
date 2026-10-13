#pragma once

// Helpers shared by the tool implementations (not part of the public interface).

#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "tbcad/geom/geom.h"
#include "tbcad/mcp/entity_json.h"
#include "tbcad/mcp/tool.h"
#include "tbcad/mcp/tools.h"

namespace tbcad::mcp::detail {

// ---- Schema builders --------------------------------------------------------------------------

inline Json withDesc(Json::Object o, const std::string& desc) {
    if (!desc.empty())
        o["description"] = desc;
    return Json(std::move(o));
}
inline Json tString(const std::string& desc = {}) {
    return withDesc({{"type", Json("string")}}, desc);
}
inline Json tNumber(const std::string& desc = {}) {
    return withDesc({{"type", Json("number")}}, desc);
}
inline Json tInt(const std::string& desc = {}, double min = 0, double max = 0) {
    Json::Object o{{"type", Json("integer")}};
    if (min != 0 || max != 0) {
        o["minimum"] = min;
        o["maximum"] = max;
    }
    return withDesc(std::move(o), desc);
}
inline Json tBool(const std::string& desc = {}) {
    return withDesc({{"type", Json("boolean")}}, desc);
}
inline Json tAny(const std::string& desc = {}) {
    return withDesc({}, desc);
}
inline Json tArray(Json items, const std::string& desc = {}, double maxItems = 0) {
    Json::Object o{{"type", Json("array")}, {"items", std::move(items)}};
    if (maxItems > 0)
        o["maxItems"] = maxItems;
    return withDesc(std::move(o), desc);
}
inline Json tEnum(std::vector<std::string> values, const std::string& desc = {}) {
    Json::Array a;
    for (auto& v : values)
        a.emplace_back(std::move(v));
    return withDesc({{"type", Json("string")}, {"enum", Json(std::move(a))}}, desc);
}
inline Json tPoint(const std::string& desc = "Point [x, y] in mm") {
    return withDesc(
        {{"type", Json("array")}, {"items", tAny()}, {"minItems", Json(2)}, {"maxItems", Json(2)}},
        desc);
}
inline Json tObject(Json::Object props, std::vector<std::string> required = {},
                    const std::string& desc = {}) {
    Json::Object o{{"type", Json("object")}, {"properties", Json(std::move(props))}};
    if (!required.empty()) {
        Json::Array r;
        for (auto& s : required)
            r.emplace_back(std::move(s));
        o["required"] = Json(std::move(r));
    }
    return withDesc(std::move(o), desc);
}
// Loose output schema: {type: object, properties: {name: {}...}}.
inline Json outSchema(std::vector<std::string> names) {
    Json::Object props;
    for (auto& n : names)
        props[n] = Json(Json::Object{});
    return tObject(std::move(props));
}

// ---- Argument access --------------------------------------------------------------------------

inline std::string argString(const Json& a, const char* key, std::string fallback = {}) {
    const Json* v = a.find(key);
    return v && v->isString() ? v->asString() : fallback;
}
inline bool argBool(const Json& a, const char* key, bool fallback = false) {
    const Json* v = a.find(key);
    return v && v->isBool() ? v->asBool() : fallback;
}
inline double argNumber(const Json& a, const char* key, double fallback) {
    const Json* v = a.find(key);
    return v && v->isNumber() ? v->asNumber() : fallback;
}

// Ids travel as decimal strings (64-bit safe); plain numbers are accepted too.
inline std::optional<EntityId> parseId(const Json& j) {
    if (j.isString()) {
        const std::string& s = j.asString();
        if (s.empty() || s.size() > 19 ||
            !std::all_of(s.begin(), s.end(), [](char c) { return c >= '0' && c <= '9'; }))
            return std::nullopt;
        return EntityId{std::stoull(s)};
    }
    if (j.isNumber() && j.asNumber() >= 1 && j.asNumber() < 9007199254740992.0)
        return EntityId{static_cast<std::uint64_t>(j.asNumber())};
    return std::nullopt;
}
inline Json idJson(EntityId id) {
    return Json(std::to_string(id.value));
}
inline Json idsJson(const std::vector<EntityId>& ids) {
    Json::Array a;
    a.reserve(ids.size());
    for (auto id : ids)
        a.push_back(idJson(id));
    return Json(std::move(a));
}
// Model-space entities only; an error names the first id that does not exist.
inline Result<std::vector<EntityId>> parseIds(const Document& doc, const Json* arr) {
    std::vector<EntityId> out;
    if (!arr)
        return out;
    for (const auto& j : arr->asArray()) {
        auto id = parseId(j);
        if (!id)
            return makeError(ErrorCode::InvalidArgument, "bad entity id " + dump(j));
        if (!doc.contains(*id) || doc.ownerBlock(*id))
            return makeError(ErrorCode::NotFound, "no entity with id " + std::to_string(id->value));
        out.push_back(*id);
    }
    return out;
}

inline std::string pointText(Vec2 p) {
    return "[" + formatDouble(p.x) + "," + formatDouble(p.y) + "]";
}

// ---- Document queries -------------------------------------------------------------------------

// Model-space entity ids in ascending order.
std::vector<EntityId> modelSpaceIds(const Document& doc);

// Bounding box of everything in model space (empty box when the drawing is empty).
BBox2 drawingExtents(DrawingSession& s);

Json bboxJson(const BBox2& b);

// Why an entity cannot be edited (locked layer, or outside the active branch in Active Only
// mode); empty when it can.
std::string whyReadOnly(const Document& doc, const Entity& e);

// Records which entities a block of work created, changed and erased.
class ChangeRecorder final : public DocumentObserver {
public:
    explicit ChangeRecorder(Document& doc) : doc_(doc) { doc_.addObserver(this); }
    ~ChangeRecorder() override { doc_.removeObserver(this); }
    ChangeRecorder(const ChangeRecorder&) = delete;
    ChangeRecorder& operator=(const ChangeRecorder&) = delete;

    void onChanged(const ChangeSet& cs) override;
    std::vector<EntityId> created() const { return pick(1); }
    std::vector<EntityId> changed() const { return pick(2); }
    std::vector<EntityId> erased() const { return pick(3); }
    bool any() const { return !state_.empty(); }

private:
    std::vector<EntityId> pick(int what) const;
    Document& doc_;
    std::map<EntityId, int> state_{};  // 1 created, 2 changed, 3 erased
};

// JSON shared by tools and resources.
Json drawingInfoJson(DrawingSession& s, const std::string& serverVersion);
Json layersJson(const Document& doc, bool includeEffective);
Json commandsJson(DrawingSession& s, const std::string& filter);

// ---- Tool registration (one function per source file) ------------------------------------------

void registerReadTools(ToolSet& tools);
void registerEditTools(ToolSet& tools);
void registerLayerTools(ToolSet& tools);
void registerAssemblyTools(ToolSet& tools);
void registerLevelTools(ToolSet& tools);
void registerFileTools(ToolSet& tools);
void registerCommandTools(ToolSet& tools);
void registerModifyTools(ToolSet& tools);
void registerLayoutTools(ToolSet& tools);
void registerBlockTools(ToolSet& tools);
void registerOpsTools(ToolSet& tools);

// "x,y" as a command input.
std::string pointInput(Vec2 p);
// Runs a command with scripted inputs and turns a failure into a tool error naming the prompts it
// saw. On success the result has messages, created and erased ids (plus `extra`).
ToolResult runChecked(DrawingSession& s, const std::string& command, std::vector<Json> inputs,
                      Json::Object extra = {});
void registerConstraintTools(ToolSet& tools);

// Result of running a command with scripted input (also used by export_pdf / export_dxf).
struct CommandRunReport {
    bool finished = false;
    std::string error{};
    std::vector<std::string> prompts{};
    std::vector<std::string> messages{};
    std::string pendingPrompt{};
};
// Feeds `inputs` (see the run_command schema) to `command`. Does not touch undo groups.
CommandRunReport runScripted(DrawingSession& s, const std::string& command,
                             const std::vector<Json>& inputs, bool allowIncomplete);

}  // namespace tbcad::mcp::detail
