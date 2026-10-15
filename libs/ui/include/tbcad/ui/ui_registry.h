#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "tbcad/cmd/host.h"
#include "tbcad/core/core.h"
#include "tbcad/render/camera.h"
#include "tbcad/render/overlay.h"

namespace tbcad::index {
class SpatialIndex;
}

namespace tbcad::ui {

// What a panel, overlay drawer or property section can see and do. The application fills it in
// every frame; none of it depends on Dear ImGui, so modules stay testable without a window.
struct PanelContext {
    Document* doc = nullptr;
    cmd::IHost* host = nullptr;
    const std::vector<EntityId>* selection = nullptr;
    const render::Camera2D* camera = nullptr;
    // Runs a command line exactly as if the user had typed it ("LAYER", "-LAYER N Walls").
    std::function<void(const std::string&)> runCommand{};
    // Snap marker / tracking line of the point prompt in progress (overlay drawers only).
    const cmd::FilterFeedback* feedback = nullptr;
    // Cursor in drawing units and the pick aperture in mm at the current zoom.
    Vec2 cursorWorld{};
    double apertureMm = 0;
    // A command is waiting for input (property panels may disable their edits meanwhile).
    bool commandActive = false;
    // Draws the property sections other modules registered for this entity's kind (dimensions,
    // hatch ...). Set by the application; empty in tests.
    std::function<void(EntityId)> drawPropertySections{};
    // True while the sections are drawn for several selected objects: a section then gets the
    // first selected object of its kind and must either handle all of `selection` or draw nothing.
    bool multiSelection = false;
    std::function<void()> drawMultiSections{};
};

// The drawing a command module works on. Passed to the session hooks whenever a different
// document becomes current, and once when the hook is installed.
struct SessionInfo {
    Document* doc = nullptr;
    const index::SpatialIndex* index = nullptr;
    cmd::IHost* host = nullptr;
};
using SessionHookFn = std::function<void(const SessionInfo&)>;

// Panels are drawn with Dear ImGui by the module that registers them: the application calls
// drawFn inside a window it created. The function may include <imgui.h> itself.
using PanelDrawFn = std::function<void(PanelContext&)>;
using PropertyDrawFn = std::function<void(PanelContext&, EntityId)>;
// Called for a left click that no command is waiting for, before the click selects anything.
// Return true when the module handled it (usually by running a command line), which stops the
// selection from changing. `world` is the click in drawing units.
using IdleClickFn =
    std::function<bool(PanelContext&, Vec2 world, double apertureMm, bool shift, bool ctrl)>;
using OverlayDrawFn = std::function<void(PanelContext&, render::Overlay&)>;

struct PanelInfo {
    std::string id{};  // unique, e.g. "layers"; also the key for the dock layout
    std::string title{};
    PanelDrawFn draw{};
    bool visibleByDefault = false;
};

struct MenuItemInfo {
    std::string path{};          // "Draw/Line" ; the last element is the label
    std::string command{};       // command line to run
    std::string actionId{};      // keybinding action id; empty = none
    std::string defaultChord{};  // "Ctrl+Shift+L" ; empty = unbound by default
};

struct StatusToggle {
    std::string id{};     // "snap"
    std::string label{};  // "SNAP"
    std::function<bool()> get{};
    std::function<void(bool)> set{};
};

struct PropertySection {
    EntityKind kind{};
    PropertyDrawFn draw{};
};

// Where command modules add their user interface. Each module's registerUi(UiRegistry&) is
// called once at start-up by the generated registerAllUi.
class UiRegistry {
public:
    // False (and nothing added) when the id is already taken or empty.
    bool addPanel(std::string id, std::string title, PanelDrawFn draw,
                  bool visibleByDefault = false);
    // `path` has at least a menu and a label ("Draw/Line"); nested menus use more slashes.
    bool addMenuItem(std::string path, std::string commandName, std::string actionId = {},
                     std::string defaultChord = {});
    bool addStatusToggle(std::string id, std::string label, std::function<bool()> get,
                         std::function<void(bool)> set);
    void addOverlayDrawer(OverlayDrawFn fn);
    // Like an overlay drawer, but drawn before the drawing so that it lies under the objects
    // (the grid).
    void addUnderlayDrawer(OverlayDrawFn fn);
    // Hooks run in registration order until one returns true.
    void addIdleClickHook(IdleClickFn fn) { idleClicks_.push_back(std::move(fn)); }
    void addPropertySection(EntityKind kind, PropertyDrawFn draw);
    // Point filters (osnap, polar/ortho, grid snap ...). The registry owns them; the lowest
    // `order` that returns a point wins. AppCore installs them into every command runner.
    void addPointFilter(int order, std::unique_ptr<cmd::PointFilter> filter);
    // Extends typed point input (direct distance ...). Not owned: the pointer must outlive the
    // application. A later call replaces the earlier one; nullptr clears it.
    void setLineParser(cmd::CommandLineParser* parser) { lineParser_ = parser; }
    // The editable fields next to the cursor (dynamic input). Not owned: the pointer must outlive
    // the application. A later call replaces the earlier one; nullptr clears it.
    void setCursorFields(cmd::CursorFieldProvider* provider) { cursorFields_ = provider; }
    // Called by AppCore each time a document becomes current (new, open, recover).
    void addSessionHook(SessionHookFn fn) {
        if (fn)
            sessionHooks_.push_back(std::move(fn));
    }

    const std::vector<PanelInfo>& panels() const { return panels_; }
    const std::vector<MenuItemInfo>& menuItems() const { return menuItems_; }
    const std::vector<StatusToggle>& statusToggles() const { return toggles_; }
    const std::vector<OverlayDrawFn>& overlayDrawers() const { return overlays_; }
    const std::vector<OverlayDrawFn>& underlayDrawers() const { return underlays_; }
    const std::vector<IdleClickFn>& idleClickHooks() const { return idleClicks_; }
    const std::vector<PropertySection>& propertySections() const { return sections_; }
    struct PointFilterEntry {
        int order = 0;
        std::unique_ptr<cmd::PointFilter> filter{};
    };
    const std::vector<PointFilterEntry>& pointFilters() const { return filters_; }
    cmd::CommandLineParser* lineParser() const { return lineParser_; }
    cmd::CursorFieldProvider* cursorFields() const { return cursorFields_; }
    const std::vector<SessionHookFn>& sessionHooks() const { return sessionHooks_; }
    // Sections registered for this entity kind, in registration order.
    std::vector<const PropertySection*> sectionsFor(EntityKind kind) const;

private:
    std::vector<PanelInfo> panels_{};
    std::vector<MenuItemInfo> menuItems_{};
    std::vector<StatusToggle> toggles_{};
    std::vector<OverlayDrawFn> overlays_{};
    std::vector<OverlayDrawFn> underlays_{};
    std::vector<IdleClickFn> idleClicks_{};
    std::vector<PropertySection> sections_{};
    std::vector<PointFilterEntry> filters_{};
    cmd::CommandLineParser* lineParser_ = nullptr;
    cmd::CursorFieldProvider* cursorFields_ = nullptr;
    std::vector<SessionHookFn> sessionHooks_{};
};

}  // namespace tbcad::ui
