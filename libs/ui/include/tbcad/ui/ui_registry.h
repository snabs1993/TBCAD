#pragma once

#include <functional>
#include <string>
#include <vector>

#include "tbcad/cmd/host.h"
#include "tbcad/core/core.h"
#include "tbcad/render/camera.h"
#include "tbcad/render/overlay.h"

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
};

// Panels are drawn with Dear ImGui by the module that registers them: the application calls
// drawFn inside a window it created. The function may include <imgui.h> itself.
using PanelDrawFn = std::function<void(PanelContext&)>;
using PropertyDrawFn = std::function<void(PanelContext&, EntityId)>;
using OverlayDrawFn = std::function<void(PanelContext&, render::Overlay&)>;

struct PanelInfo {
    std::string id{};  // unique, e.g. "layers"; also the key for the dock layout
    std::string title{};
    PanelDrawFn draw{};
    bool visibleByDefault = false;
};

struct MenuItemInfo {
    std::string path{};     // "Draw/Line" ; the last element is the label
    std::string command{};  // command line to run
    std::string actionId{};      // keybinding action id; empty = none
    std::string defaultChord{};  // "Ctrl+Shift+L" ; empty = unbound by default
};

struct StatusToggle {
    std::string id{};  // "snap"
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
    void addPropertySection(EntityKind kind, PropertyDrawFn draw);

    const std::vector<PanelInfo>& panels() const { return panels_; }
    const std::vector<MenuItemInfo>& menuItems() const { return menuItems_; }
    const std::vector<StatusToggle>& statusToggles() const { return toggles_; }
    const std::vector<OverlayDrawFn>& overlayDrawers() const { return overlays_; }
    const std::vector<PropertySection>& propertySections() const { return sections_; }
    // Sections registered for this entity kind, in registration order.
    std::vector<const PropertySection*> sectionsFor(EntityKind kind) const;

private:
    std::vector<PanelInfo> panels_{};
    std::vector<MenuItemInfo> menuItems_{};
    std::vector<StatusToggle> toggles_{};
    std::vector<OverlayDrawFn> overlays_{};
    std::vector<PropertySection> sections_{};
};

}  // namespace tbcad::ui
