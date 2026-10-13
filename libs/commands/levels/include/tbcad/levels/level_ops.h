#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "tbcad/core/core.h"

namespace tbcad::levels {

// Headless level operations (see build/levels-brief.md). Every function reads the document (which
// already shows the transaction's pending edits) and edits through the caller's Transaction, so
// a command or a test decides what one undo step contains. Errors are fit to show to the user.

// A level reference: "#12" (id) or the level's name (case-insensitive).
Result<LevelId> resolveLevel(const Document& doc, std::string_view ref);
// Levels sorted by elevation, then id.
std::vector<LevelId> levelOrder(const Document& doc);
// Number of objects (model space and block definitions) on the level.
std::size_t entityCount(const Document& doc, LevelId id);

Result<LevelId> createLevel(const Document& doc, Transaction& tx, std::string name,
                            double elevation);
Result<void> renameLevel(const Document& doc, Transaction& tx, LevelId id, std::string newName);
Result<void> setElevation(const Document& doc, Transaction& tx, LevelId id, double elevation);
Result<void> setVisible(const Document& doc, Transaction& tx, LevelId id, bool visible);
Result<void> setLocked(const Document& doc, Transaction& tx, LevelId id, bool locked);
// Makes the level current. A hidden level is shown by this.
Result<void> setCurrentLevel(const Document& doc, Transaction& tx, LevelId id);
Result<void> setLevelView(const Document& doc, Transaction& tx, LevelView view);
// Moves model-space objects to `level`; objects inside block definitions are skipped. Returns the
// number moved.
Result<std::size_t> moveToLevel(const Document& doc, Transaction& tx,
                                const std::vector<EntityId>& ids, LevelId level);
// Moves the level's objects to `moveTo` (default: the default level) and deletes it. The default
// level cannot be deleted. When the level is current, `moveTo` becomes current.
Result<void> deleteLevel(const Document& doc, Transaction& tx, LevelId id,
                         LevelId moveTo = kLevelDefault);

// "Level 2 (elevation 3000 mm)" style label for lists.
std::string describe(const Document& doc, LevelId id);
// Parses "dim" / "hide" / "all".
Result<LevelView> parseLevelView(std::string_view text);
const char* levelViewName(LevelView view);

}  // namespace tbcad::levels
