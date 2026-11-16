#pragma once

#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "tbcad/block_cmds/block_cmds.h"
#include "tbcad/cmd/cmd.h"
#include "tbcad/drafting/snap_geometry.h"

namespace tbcad::block_cmds {

// The Attach dialog (XATTACH, PDFATTACH, IMAGEATTACH in a window). The command fills
// kAttachFieldsSetting, opens panel "xattachdlg" and waits; Attach stores the choices in
// kAttachResultSetting and presses Enter.
inline constexpr const char* kAttachFieldsSetting = "tmp.xattach.fields";
inline constexpr const char* kAttachResultSetting = "tmp.xattach.result";
struct AttachAnswers {
    std::string name{};
    bool overlay = false;
    char path = 'R';  // R relative, A absolute, N file name only
    int page = 1;     // PDF page
    double fade = 0;  // percent, PDF pages and pictures
    double scale = 1;
    double rotationDeg = 0;
};
struct AttachFields {
    std::string kind{};  // "drawing", "pdf" or "image"
    std::string file{};  // the file name shown
    int pages = 1;
    bool saved = true;  // the drawing has a file (a relative path can be stored)
    AttachAnswers defaults{};
};
std::string encodeAttachFields(const std::string& kind, const std::u8string& file, int pages,
                               const AttachAnswers& defaults, bool saved);
std::optional<AttachFields> decodeAttachFields(const std::string& text);
std::string encodeAttachResult(const AttachAnswers& a);
std::optional<AttachAnswers> decodeAttachResult(const std::string& text);

std::unique_ptr<cmd::Command> makeBlockCommand();
std::unique_ptr<cmd::Command> makeBlockLineCommand();
std::unique_ptr<cmd::Command> makeAttredefCommand();
std::unique_ptr<cmd::Command> makeBaseCommand();
std::unique_ptr<cmd::Command> makeBEditCommand();
std::unique_ptr<cmd::Command> makeBSaveCommand(bool close);
std::unique_ptr<cmd::Command> makeRefSetCommand();
std::unique_ptr<cmd::Command> makeBSaveAsCommand();
std::unique_ptr<cmd::Command> makeBvStateCommand();
std::unique_ptr<cmd::Command> makeBvHideShowCommand(bool show);
std::unique_ptr<cmd::Command> makeBLookupTableCommand();
std::unique_ptr<cmd::Command> makeBTestBlockCommand();
std::unique_ptr<cmd::Command> makeBAttOrderCommand();
std::unique_ptr<cmd::Command> makeInsertCommand();
std::unique_ptr<cmd::Command> makeTypedInsertCommand();
std::unique_ptr<cmd::Command> makeBlocksPaletteCommand();
std::unique_ptr<cmd::Command> makeBlocksPaletteCloseCommand();
std::unique_ptr<cmd::Command> makeMinsertCommand();
std::unique_ptr<cmd::Command> makeExplodeCommand();
std::unique_ptr<cmd::Command> makeXplodeCommand();
std::unique_ptr<cmd::Command> makeBurstCommand();
std::unique_ptr<cmd::Command> makeNCopyCommand();
std::unique_ptr<cmd::Command> makeArrayCommand();
std::unique_ptr<cmd::Command> makeArrayLegacyCommand();
std::unique_ptr<cmd::Command> makeArrayRectCommand();
std::unique_ptr<cmd::Command> makeArrayPolarCommand();
std::unique_ptr<cmd::Command> makeArrayPathCommand();
std::unique_ptr<cmd::Command> makeArrayEditCommand();
std::unique_ptr<cmd::Command> makeArrayRemoveCommand();
std::unique_ptr<cmd::Command> makeBParamCommand();
std::unique_ptr<cmd::Command> makeBActionCommand();
std::unique_ptr<cmd::Command> makeDynSetCommand();
std::unique_ptr<cmd::Command> makeResetBlockCommand();
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
std::unique_ptr<cmd::Command> makeXrefLineCommand();
std::unique_ptr<cmd::Command> makeXBindCommand();
std::unique_ptr<cmd::Command> makeXClipCommand();
std::unique_ptr<cmd::Command> makeClipFrameCommand(bool pdf);
std::unique_ptr<cmd::Command> makeXrefFadeCommand();
std::unique_ptr<cmd::Command> makePdfClipCommand();
std::unique_ptr<cmd::Command> makeXUnloadCommand();
std::unique_ptr<cmd::Command> makeXListCommand();
std::unique_ptr<cmd::Command> makeXrefTypeCommand();
std::unique_ptr<cmd::Command> makeXOpenCommand();
std::unique_ptr<cmd::Command> makeXrefPaletteCommand();
std::unique_ptr<cmd::Command> makeXPathCommand();
std::unique_ptr<cmd::Command> makeXRelinkCommand();
std::unique_ptr<cmd::Command> makeBlockCountCommand();
std::unique_ptr<cmd::Command> makeBlockReplaceCommand();
void registerBlockPanel(ui::UiRegistry& ui);
// The Purge dialog ("purge") and its File menu entry.
void registerPurgePanel(ui::UiRegistry& ui);
void registerSelectPanels(ui::UiRegistry& ui);
void registerBlockAuthoringPanel(ui::UiRegistry& ui);
void registerXrefPanel(ui::UiRegistry& ui);

namespace detail {

// Selected ids that still exist in model space, in draw order (back to front).
std::vector<EntityId> inDrawOrder(const Document& doc, const std::vector<EntityId>& ids);

// Layers that are off, frozen or locked cannot be edited by these commands.
bool editable(const Document& doc, const Entity& e);

// Editable objects in `owner`, the space being drawn in (model space, or a layout sheet's own
// objects), so BLOCK, ARRAY and EXPLODE work on a sheet too.
cmd::EntityFilterFn editableFilter(const Document& doc, BlockId owner);

// Union of the geometry bounds of the entities.
BBox2 boundsOf(const Document& doc, const std::vector<Entity>& entities);

// "3 objects" / "1 object".
std::string objects(std::size_t n);

// Starts editing `def` in place (BEDIT): copies of its objects go into the drawing through `xf`
// (block space to drawing). False, with a message, when another block is being edited or `def`
// is an external reference.
bool startBlockEdit(cmd::CommandContext& ctx, const BlockDef& def, const Transform2& xf);

// The block being edited with BEDIT: `xf` takes block space to the drawing, `working` is the
// working set (copies in the drawing, back to front) and `state` the visibility state shown.
struct BlockEdit {
    BlockId block{};
    Transform2 xf{};
    std::vector<EntityId> working{};
    std::string state{};
    bool xref = false;  // REFEDIT of an external reference: saving writes its file
};
std::optional<BlockEdit> blockEdit(const Document& doc, const cmd::IHost& host);
// BSAVE without closing: afterwards every working copy stands for a block entity. With `into`
// (BSAVEAS: a new block holding a copy of the definition) the working set goes there instead and
// the edit goes on with that block.
bool saveBlockEdit(cmd::CommandContext& ctx, BlockId into = {});
// Working copies <-> the block entities they stand for (after the last save).
std::vector<EntityId> blockIdsOf(const cmd::IHost& host, const std::vector<EntityId>& copies);
std::vector<EntityId> copiesOf(const cmd::IHost& host, const std::vector<EntityId>& blockIds);
// Shows the working set as visibility state `state` of the block (others hidden).
void showEditState(cmd::CommandContext& ctx, const std::string& state);
// BTESTBLOCK references: left out of the working set, erased when the edit ends.
void addTestReference(cmd::CommandContext& ctx, EntityId id);
std::vector<EntityId> testReferences(const cmd::IHost& host);
void closeTestReferences(cmd::CommandContext& ctx);

// The associative array (in model space, on an editable layer) and its item under `p`, within
// `tol` of the item's extents.
std::optional<std::pair<EntityId, int>> arrayItemNear(const Document& doc, Vec2 p, double tol);

}  // namespace detail

}  // namespace tbcad::block_cmds
