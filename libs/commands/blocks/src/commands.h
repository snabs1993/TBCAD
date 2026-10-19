#pragma once

#include <memory>
#include <optional>
#include <utility>
#include <string>
#include <vector>

#include "tbcad/block_cmds/block_cmds.h"
#include "tbcad/cmd/cmd.h"
#include "tbcad/drafting/snap_geometry.h"

namespace tbcad::block_cmds {

std::unique_ptr<cmd::Command> makeBlockCommand();
std::unique_ptr<cmd::Command> makeAttredefCommand();
std::unique_ptr<cmd::Command> makeBaseCommand();
std::unique_ptr<cmd::Command> makeBEditCommand();
std::unique_ptr<cmd::Command> makeBSaveCommand(bool close);
std::unique_ptr<cmd::Command> makeRefSetCommand();
std::unique_ptr<cmd::Command> makeInsertCommand();
std::unique_ptr<cmd::Command> makeMinsertCommand();
std::unique_ptr<cmd::Command> makeExplodeCommand();
std::unique_ptr<cmd::Command> makeXplodeCommand();
std::unique_ptr<cmd::Command> makeBurstCommand();
std::unique_ptr<cmd::Command> makeNCopyCommand();
std::unique_ptr<cmd::Command> makeArrayCommand();
std::unique_ptr<cmd::Command> makeArrayRectCommand();
std::unique_ptr<cmd::Command> makeArrayPolarCommand();
std::unique_ptr<cmd::Command> makeArrayPathCommand();
std::unique_ptr<cmd::Command> makeArrayEditCommand();
std::unique_ptr<cmd::Command> makeArrayRemoveCommand();
std::unique_ptr<cmd::Command> makeBParamCommand();
std::unique_ptr<cmd::Command> makeBActionCommand();
std::unique_ptr<cmd::Command> makeDynSetCommand();
std::unique_ptr<cmd::Command> makeAsmDefCommand();
std::unique_ptr<cmd::Command> makeAsmSetCommand();
std::unique_ptr<cmd::Command> makeAsmInfoCommand();
std::unique_ptr<cmd::Command> makeAsmBomCommand();
std::unique_ptr<cmd::Command> makeJoinAsmCommand(bool join);
std::unique_ptr<cmd::Command> makeAsmSetSelectedCommand();
std::unique_ptr<cmd::Command> makeAttDefCommand();
std::unique_ptr<cmd::Command> makeAttDefLineCommand();
std::unique_ptr<cmd::Command> makeAttEditCommand();
std::unique_ptr<cmd::Command> makeAttExtCommand();
std::unique_ptr<cmd::Command> makeAttInCommand();
std::unique_ptr<cmd::Command> makeAttTableCommand();
std::unique_ptr<cmd::Command> makeAttSyncCommand();
std::unique_ptr<cmd::Command> makeBattManCommand();
std::unique_ptr<cmd::Command> makeXAttachCommand();
std::unique_ptr<cmd::Command> makePdfAttachCommand();
std::unique_ptr<cmd::Command> makeImageAttachCommand();
std::unique_ptr<cmd::Command> makeImageClipCommand();
std::unique_ptr<cmd::Command> makeImageAdjustCommand();
std::unique_ptr<cmd::Command> makeImageFrameCommand();
std::unique_ptr<cmd::Command> makeImageTransparencyCommand();
std::unique_ptr<cmd::Command> makePdfOsnapCommand();
std::unique_ptr<cmd::Command> makeUnderlayFadeCommand();
std::unique_ptr<cmd::Command> makeXReloadCommand();
std::unique_ptr<cmd::Command> makeXDetachCommand();
std::unique_ptr<cmd::Command> makeXBindCommand();
std::unique_ptr<cmd::Command> makeXClipCommand();
std::unique_ptr<cmd::Command> makeXUnloadCommand();
std::unique_ptr<cmd::Command> makeXListCommand();
std::unique_ptr<cmd::Command> makeETransmitCommand();
std::unique_ptr<cmd::Command> makeXrefPaletteCommand();
std::unique_ptr<cmd::Command> makeXPathCommand();
std::unique_ptr<cmd::Command> makeXRelinkCommand();
std::unique_ptr<cmd::Command> makeBlockCountCommand();
std::unique_ptr<cmd::Command> makeBlockReplaceCommand();
void registerBlockPanel(ui::UiRegistry& ui);
void registerXrefPanel(ui::UiRegistry& ui);

namespace detail {

// Selected ids that still exist in model space, in draw order (back to front).
std::vector<EntityId> inDrawOrder(const Document& doc, const std::vector<EntityId>& ids);

// Layers that are off, frozen or locked cannot be edited by these commands.
bool editable(const Document& doc, const Entity& e);

// Selection filter shared by the commands: model-space entities on editable layers.
cmd::EntityFilterFn editableFilter(const Document& doc);

// Union of the geometry bounds of the entities.
BBox2 boundsOf(const Document& doc, const std::vector<Entity>& entities);

// "3 objects" / "1 object".
std::string objects(std::size_t n);

// Starts editing `def` in place (BEDIT): copies of its objects go into the drawing through `xf`
// (block space to drawing). False, with a message, when another block is being edited or `def`
// is an external reference.
bool startBlockEdit(cmd::CommandContext& ctx, const BlockDef& def, const Transform2& xf);

// The associative array (in model space, on an editable layer) and its item under `p`, within
// `tol` of the item's extents.
std::optional<std::pair<EntityId, int>> arrayItemNear(const Document& doc, Vec2 p, double tol);

}  // namespace detail

}  // namespace tbcad::block_cmds
