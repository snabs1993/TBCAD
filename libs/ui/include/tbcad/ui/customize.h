#pragma once

// What the user can change about the interface besides settings, as AutoCAD's CUI does: the
// double-click action of each object type, the Quick Access toolbar and the extra commands of
// the right-click menu. Kept in the settings file; command aliases (aliases.json, acad.pgp) and
// keyboard shortcuts (keybindings.json) have their own files.

#include <string>
#include <vector>

#include "tbcad/core/document.h"
#include "tbcad/platform/settings.h"

namespace tbcad::ui {

// ---- double-click actions (DBLCLKEDIT) ------------------------------------------------------

// The command a double click runs: a command line, or kOpenProperties for the Properties panel.
inline constexpr const char* kOpenProperties = "PROPERTIES";
// DBLCLKEDIT: 0 turns double-click editing off (a double click only selects).
inline constexpr const char* kDoubleClickEditKey = "selection.dblclkedit";

struct DoubleClickType {
    const char* key;            // setting "dblclick.<key>"
    const char* label;          // shown in the Customize page
    const char* defaultAction;  // as AutoCAD's acad.cuix sets it
};
const std::vector<DoubleClickType>& doubleClickTypes();

// The type `e` belongs to for double clicks: "Line", "Text", "Block", "AttributeBlock", "Xref",
// ... (one of doubleClickTypes()).
std::string doubleClickKey(const Document& doc, const Entity& e);
// The action for double-clicking `e`: the user's setting, else the default.
std::string doubleClickAction(const platform::Settings& s, const Document& doc, const Entity& e);

// ---- command lists --------------------------------------------------------------------------

// Command lines on the Quick Access toolbar, in order.
inline constexpr const char* kQuickAccessKey = "ui.quickAccess";
// Extra commands in the drawing's right-click menu with nothing selected, and with a selection.
inline constexpr const char* kContextMenuKey = "ui.contextMenu";
inline constexpr const char* kContextMenuEditKey = "ui.contextMenuEdit";

// A button or menu label for a command line: its first word, upper case ("LINE").
std::string commandLabel(const std::string& line);

}  // namespace tbcad::ui
