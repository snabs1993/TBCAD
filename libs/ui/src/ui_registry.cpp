#include "tbcad/ui/ui_registry.h"

#include <algorithm>

namespace tbcad::ui {

bool UiRegistry::addPanel(std::string id, std::string title, PanelDrawFn draw,
                          bool visibleByDefault) {
    if (id.empty() || !draw)
        return false;
    if (std::any_of(panels_.begin(), panels_.end(), [&](const PanelInfo& p) { return p.id == id; }))
        return false;
    panels_.push_back({std::move(id), std::move(title), std::move(draw), visibleByDefault});
    return true;
}

bool UiRegistry::addMenuItem(std::string path, std::string commandName, std::string actionId,
                             std::string defaultChord) {
    // Needs "Menu/Label" at least, with no empty element.
    auto slash = path.find('/');
    if (slash == std::string::npos || slash == 0 || slash + 1 >= path.size() ||
        path.find("//") != std::string::npos || path.back() == '/' || commandName.empty())
        return false;
    menuItems_.push_back(
        {std::move(path), std::move(commandName), std::move(actionId), std::move(defaultChord)});
    return true;
}

bool UiRegistry::addStatusToggle(std::string id, std::string label, std::function<bool()> get,
                                 std::function<void(bool)> set) {
    if (id.empty() || !get || !set)
        return false;
    if (std::any_of(toggles_.begin(), toggles_.end(),
                    [&](const StatusToggle& t) { return t.id == id; }))
        return false;
    toggles_.push_back({std::move(id), std::move(label), std::move(get), std::move(set)});
    return true;
}

void UiRegistry::addOverlayDrawer(OverlayDrawFn fn) {
    if (fn)
        overlays_.push_back(std::move(fn));
}

void UiRegistry::addPropertySection(EntityKind kind, PropertyDrawFn draw) {
    if (draw)
        sections_.push_back({kind, std::move(draw)});
}

std::vector<const PropertySection*> UiRegistry::sectionsFor(EntityKind kind) const {
    std::vector<const PropertySection*> out;
    for (const auto& s : sections_)
        if (s.kind == kind)
            out.push_back(&s);
    return out;
}

}  // namespace tbcad::ui
