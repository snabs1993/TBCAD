#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "tbcad/core/core.h"

namespace tbcad::layers {

// Named layer states and layer filters (Q32). Both are stored in the document header as
// `custom` keys, so they are saved in .tbcad and every change is undoable:
//   tbcad.layerstate.<NAME>   one line per layer: path, flags, colour, linetype, lineweight
//   tbcad.layerfilter.<NAME>  the filter expression text
// Names are case-insensitive; a name may not be empty or contain control characters.

// ---- layer states -----------------------------------------------------------------------

// Which parts of a layer a restore applies.
struct StateParts {
    bool visibility = true;  // on / frozen
    bool lock = true;
    bool plot = true;
    bool look = true;         // colour, linetype, lineweight
    bool current = true;      // the current layer saved with the state
    bool hideOthers = false;  // turn off layers that were not saved (AutoCAD's "turn off layers
                              // not found in layer state")
};

// Names of the saved states, sorted.
std::vector<std::string> layerStateNames(const Document& doc);
// Saves (or replaces) the state of every layer under `name`.
Result<void> saveLayerState(const Document& doc, Transaction& tx, std::string_view name);
// Applies the saved state to the layers that still exist (matched by path). Layers added after
// the save are left alone. `skipped` receives the number of saved layers that no longer exist or
// whose values could not be applied.
Result<void> restoreLayerState(const Document& doc, Transaction& tx, std::string_view name,
                               StateParts parts = {}, std::size_t* skipped = nullptr);
Result<void> deleteLayerState(const Document& doc, Transaction& tx, std::string_view name);
// A saved state as a text file body ("TBCAD-LAYERSTATE 1" header then the stored lines), for
// sharing between drawings; importLayerStateText stores such text under `name` (replacing any
// state of that name) after checking every line.
Result<std::string> exportLayerStateText(const Document& doc, std::string_view name);
Result<void> importLayerStateText(const Document& doc, Transaction& tx, std::string_view name,
                                  std::string_view text);
Result<void> renameLayerState(const Document& doc, Transaction& tx, std::string_view from,
                              std::string_view to);

// ---- layer filters ----------------------------------------------------------------------

// An expression is words separated by spaces; a layer matches when every word matches.
//   on off frozen thawed locked unlocked plot noplot   the layer's own switches
//   used empty                                         holds entities / holds none
//   name:PATTERN       layer name matches (* and ?, case-insensitive)
//   path:PATTERN       full path (Parent~Child) matches
//   branch:LAYER       the layer or anything below it
//   color:C linetype:L lineweight:W    effective values equal (C and W as in -LAYER, L a name)
//   current            the current layer
//   group:NAME         member of the saved group NAME (see "layer groups" below)
//   filter:NAME        matches the saved filter NAME (filters may use other filters)
//   not:WORD           the opposite of any word above
// A word with no colon and no keyword is a name pattern. "" or "*" matches every layer.
bool layerMatchesFilter(const Document& doc, LayerId id, std::string_view expression,
                        std::string* error = nullptr);
// Matching layers in tree order; an error for a word that cannot be understood.
Result<std::vector<LayerId>> filterLayers(const Document& doc, std::string_view expression);

// Wildcard match (* and ?), case-insensitive.
bool wildcardMatch(std::string_view pattern, std::string_view text);

std::vector<std::string> layerFilterNames(const Document& doc);
// Expression of a saved filter, or an error when there is none.
Result<std::string> savedFilterExpression(const Document& doc, std::string_view name);
// Checks the expression, then saves it.
Result<void> saveLayerFilter(const Document& doc, Transaction& tx, std::string_view name,
                             std::string_view expression);
Result<void> deleteLayerFilter(const Document& doc, Transaction& tx, std::string_view name);

// ---- layer groups -----------------------------------------------------------------------

// A group filter is a named list of layers picked by hand (AutoCAD's "group filter"), kept as
//   tbcad.layergroup.<NAME>   one layer path per line
// Members are matched by path, so a renamed or deleted layer simply drops out.
std::vector<std::string> layerGroupNames(const Document& doc);
// The members that still exist, in tree order.
Result<std::vector<LayerId>> layerGroupMembers(const Document& doc, std::string_view name);
// Saves (or replaces) the group.
Result<void> saveLayerGroup(const Document& doc, Transaction& tx, std::string_view name,
                            const std::vector<LayerId>& members);
Result<void> addToLayerGroup(const Document& doc, Transaction& tx, std::string_view name,
                             const std::vector<LayerId>& more);
Result<void> removeFromLayerGroup(const Document& doc, Transaction& tx, std::string_view name,
                                  const std::vector<LayerId>& gone);
Result<void> deleteLayerGroup(const Document& doc, Transaction& tx, std::string_view name);

}  // namespace tbcad::layers
