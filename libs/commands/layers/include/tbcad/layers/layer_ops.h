#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "tbcad/core/core.h"

namespace tbcad::layers {

// Headless layer and sub-layer operations. Every function takes the document (read side; it
// already shows the transaction's pending edits) and edits through the caller's Transaction, so a
// command (or a test) decides what one undo step contains. Errors carry a message that is fit to
// show to the user.

inline constexpr char kPathSeparator = '~';  // S-FLOOR~edge, also used for DXF names

// Full path of a layer: names joined by '~' from the top level down.
std::string layerPath(const Document& doc, LayerId id);
// Number of ancestors (0 for a top-level layer).
int layerDepth(const Document& doc, LayerId id);
// Direct children, sorted by name (case-insensitive).
std::vector<LayerId> childrenOf(const Document& doc, LayerId parent);
// Every layer, depth-first with parents before children and siblings sorted by name.
std::vector<LayerId> layerTreeOrder(const Document& doc);
// Number of entities (model space and block definitions) on the layer itself.
std::size_t entityCount(const Document& doc, LayerId id);
// `id` and all layers below it, parents first.
std::vector<LayerId> branchOf(const Document& doc, LayerId id);

// A layer reference: "#12" (id), "Walls" (top level) or "S-FLOOR~edge" (path). A bare name with
// no separator also matches a unique sub-layer anywhere when no top-level layer has that name.
Result<LayerId> resolveLayer(const Document& doc, std::string_view ref);
// Comma separated list of references; "*" is every layer.
Result<std::vector<LayerId>> resolveLayers(const Document& doc, std::string_view refs);

// New layer or sub-layer (parent null = top level). Sub-layers start with ByParent colour,
// linetype and lineweight; top-level layers start with white, Continuous, Default.
Result<LayerId> createLayer(const Document& doc, Transaction& tx, std::string name,
                            LayerId parent = {});
Result<void> renameLayer(const Document& doc, Transaction& tx, LayerId id, std::string newName);
// Moves `id` under `newParent` (null = top level). Refuses cycles, depth > 8, layer 0 and name
// clashes among the new siblings. ByParent values follow the new parent, and become fixed values
// (what the layer showed so far) when the layer goes top level.
// Empty when `id` can move under `newParent` (null = top level); otherwise the reason, fit to show
// to the user (cycle, layer 0, depth limit, name clash, already there). Reads only.
std::string whyCannotReparent(const Document& doc, LayerId id, LayerId newParent);
Result<void> reparentLayer(const Document& doc, Transaction& tx, LayerId id, LayerId newParent);

enum class DeleteMode {
    LeafOnly,        // fails when the layer has sub-layers
    Branch,          // the layer and everything below it; every one of them must be empty
    MoveChildrenUp,  // the layer only; its sub-layers move up one level and keep their look
};
// Fails (changing nothing) for layer 0, the current layer, a layer that still holds entities,
// and, in LeafOnly mode, a layer with sub-layers.
Result<void> deleteLayer(const Document& doc, Transaction& tx, LayerId id, DeleteMode mode);

// "Active only" mode: layers outside the current layer's branch behave as locked.
Result<void> setActiveOnly(const Document& doc, Transaction& tx, bool on);

Result<void> setCurrentLayer(const Document& doc, Transaction& tx, LayerId id);
// LAYERP: puts the layer settings (on, frozen, locked, plot, colour, linetype, lineweight,
// transparency) back to what they were before the last transaction that changed them.
Result<void> restorePreviousLayers(const Document& doc, Transaction& tx);
Result<void> setOn(const Document& doc, Transaction& tx, LayerId id, bool on);
Result<void> setFrozen(const Document& doc, Transaction& tx, LayerId id, bool frozen);
Result<void> setLocked(const Document& doc, Transaction& tx, LayerId id, bool locked);
Result<void> setPlot(const Document& doc, Transaction& tx, LayerId id, bool plot);
// ByLayer and ByBlock are refused; ByParent only on sub-layers.
Result<void> setLayerColor(const Document& doc, Transaction& tx, LayerId id, Color color);
Result<void> setLayerLinetype(const Document& doc, Transaction& tx, LayerId id,
                              LinetypeId linetype);
Result<void> setLayerLineweight(const Document& doc, Transaction& tx, LayerId id,
                                LineWeight weight);
Result<void> setLayerTransparency(const Document& doc, Transaction& tx, LayerId id, int percent);
// The note shown in the Description column; empty clears it. No control characters.
Result<void> setLayerDescription(const Document& doc, Transaction& tx, LayerId id,
                                 std::string_view text);
// Turns off every layer except `keep`, its ancestors and its descendants.
Result<void> isolateLayer(const Document& doc, Transaction& tx, LayerId keep);
// Same, keeping several layers (each with its ancestors and descendants). With `lockOthers` the
// other layers stay on but are locked, so they show faded (LAYISO's "Lock and fade").
Result<void> isolateLayers(const Document& doc, Transaction& tx, const std::vector<LayerId>& keep,
                           bool lockOthers = false);
// Moves every object on `from` to `into`, then deletes `from`. Refuses layer 0 and a layer with
// sub-layers. If `from` is current, `into` becomes current.
Result<void> mergeLayer(const Document& doc, Transaction& tx, LayerId from, LayerId into);
// Turns every layer on.
Result<void> showAllLayers(const Document& doc, Transaction& tx);

// ---- text <-> value helpers shared by -LAYER, -PROP and the panels
// -------------------------------

// "red".."white" (or "7"), "1".."255", "r,g,b", "ByLayer", "ByBlock", "ByParent"
// (case-insensitive).
Result<Color> parseColor(std::string_view text);
std::string colorName(Color c);
// Millimetres ("0.25"), "Default", "ByLayer", "ByBlock", "ByParent".
Result<LineWeight> parseLineweight(std::string_view text);
std::string lineweightName(LineWeight w);
// "ByLayer", "ByBlock" or 0 to 90 (a trailing % is allowed); returns the value as stored in
// EntityCommon::transparency.
Result<int> parseTransparency(std::string_view text);
std::string transparencyName(int v);
// Linetype by name, "#id", "ByLayer", "ByBlock" or "ByParent".
Result<LinetypeId> parseLinetype(const Document& doc, std::string_view text);
// Like parseLinetype, but a linetype that ships with TBCAD ("Dash", "Center", ...) and is not in
// the drawing yet is loaded into it first, so typing its name just works.
Result<LinetypeId> resolveLinetype(const Document& doc, Transaction& tx, std::string_view text);
std::string linetypeName(const Document& doc, LinetypeId id);

}  // namespace tbcad::layers
