#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "tbcad/cmd/cmd.h"
#include "tbcad/modify/modify.h"

namespace tbcad::modifycmd {

// ---- Settings (kept by the host, keys are lower-case dotted names) -----------------------------

double getSetting(const cmd::IHost& host, std::string_view key, double fallback);
void putSetting(cmd::IHost& host, std::string_view key, double value);
bool getFlag(const cmd::IHost& host, std::string_view key, bool fallback);
void putFlag(cmd::IHost& host, std::string_view key, bool value);

// ---- Entities ----------------------------------------------------------------------------------

// Entities the editing commands can use as sources / edges: curves on layers that are on, not
// frozen and not locked (sources) and visible.
bool isEditable(const Document& doc, const Entity& e);
bool isUsableEdge(const Document& doc, const Entity& e);
// Any entity kind on a layer that is on, thawed and unlocked (for commands that do not need curve
// geometry: ALIGN, MATCHPROP, SETBYLAYER, SCALETEXT).
bool isModifiable(const Document& doc, const Entity& e);

// Applies an algorithm's edit to the entity `id` in the command's transaction. Returns the ids of
// the entities now standing for the source (the modified one first, then the added ones).
Result<std::vector<EntityId>> applyEdit(Transaction& tx, EntityId id, modify::EntityEdit edit);
Result<void> replaceEntity(Transaction& tx, EntityId id, const Entity& value);

// Draws an entity into a preview list at the current zoom.
void previewEntity(const Document& doc, const cmd::IHost& host, const Entity& e, DisplayList& out);

}  // namespace tbcad::modifycmd
