#pragma once

#include <memory>
#include <string>
#include <vector>

#include "tbcad/block_cmds/block_cmds.h"
#include "tbcad/cmd/cmd.h"
#include "tbcad/drafting/snap_geometry.h"

namespace tbcad::block_cmds {

std::unique_ptr<cmd::Command> makeBlockCommand();
std::unique_ptr<cmd::Command> makeInsertCommand();
std::unique_ptr<cmd::Command> makeExplodeCommand();
std::unique_ptr<cmd::Command> makeArrayCommand();
std::unique_ptr<cmd::Command> makeArrayRectCommand();
std::unique_ptr<cmd::Command> makeArrayPolarCommand();
std::unique_ptr<cmd::Command> makeArrayPathCommand();
std::unique_ptr<cmd::Command> makeArrayEditCommand();
std::unique_ptr<cmd::Command> makeBParamCommand();
std::unique_ptr<cmd::Command> makeBActionCommand();
std::unique_ptr<cmd::Command> makeDynSetCommand();
std::unique_ptr<cmd::Command> makeAsmDefCommand();
std::unique_ptr<cmd::Command> makeAsmSetCommand();
std::unique_ptr<cmd::Command> makeAsmInfoCommand();
std::unique_ptr<cmd::Command> makeJoinAsmCommand(bool join);
std::unique_ptr<cmd::Command> makeAsmSetSelectedCommand();
std::unique_ptr<cmd::Command> makeAttDefCommand();
std::unique_ptr<cmd::Command> makeAttEditCommand();
std::unique_ptr<cmd::Command> makeAttSyncCommand();
std::unique_ptr<cmd::Command> makeXAttachCommand();
std::unique_ptr<cmd::Command> makeXReloadCommand();
std::unique_ptr<cmd::Command> makeXDetachCommand();
std::unique_ptr<cmd::Command> makeXBindCommand();
std::unique_ptr<cmd::Command> makeXClipCommand();
std::unique_ptr<cmd::Command> makeXUnloadCommand();
std::unique_ptr<cmd::Command> makeXListCommand();
std::unique_ptr<cmd::Command> makeBlockCountCommand();
void registerBlockPanel(ui::UiRegistry& ui);

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

}  // namespace detail

}  // namespace tbcad::block_cmds
