#include "tbcad/ui/ui_registry.h"

#include <algorithm>
#include <map>
#include <string>

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

bool UiRegistry::addContextualTab(std::string id, std::string title, std::vector<EntityKind> kinds,
                                  PanelDrawFn draw) {
    if (id.empty() || kinds.empty() || !draw)
        return false;
    if (std::any_of(contextTabs_.begin(), contextTabs_.end(),
                    [&](const ContextualTab& t) { return t.id == id; }))
        return false;
    contextTabs_.push_back({std::move(id), std::move(title), std::move(kinds), std::move(draw)});
    return true;
}

const ContextualTab* UiRegistry::contextualTabFor(const Document& doc,
                                                  const std::vector<EntityId>& selection) const {
    if (selection.empty() || selection.size() > kMaxContextualSelection || contextTabs_.empty())
        return nullptr;
    // The kinds present, once each.
    std::vector<EntityKind> present;
    for (EntityId id : selection) {
        const Entity* e = doc.find(id);
        if (!e)
            continue;
        const EntityKind k = kind(*e);
        if (std::find(present.begin(), present.end(), k) == present.end())
            present.push_back(k);
    }
    if (present.empty())
        return nullptr;
    for (const ContextualTab& t : contextTabs_) {
        if (std::all_of(present.begin(), present.end(), [&](EntityKind k) {
                return std::find(t.kinds.begin(), t.kinds.end(), k) != t.kinds.end();
            }))
            return &t;
    }
    return nullptr;
}

void UiRegistry::addOverlayDrawer(OverlayDrawFn fn) {
    if (fn)
        overlays_.push_back(std::move(fn));
}

void UiRegistry::addUnderlayDrawer(OverlayDrawFn fn) {
    if (fn)
        underlays_.push_back(std::move(fn));
}

void UiRegistry::addPropertySection(EntityKind kind, PropertyDrawFn draw) {
    if (draw)
        sections_.push_back({kind, std::move(draw)});
}

void UiRegistry::addPointFilter(int order, std::unique_ptr<cmd::PointFilter> filter) {
    if (filter)
        filters_.push_back({order, std::move(filter)});
}

std::vector<const PropertySection*> UiRegistry::sectionsFor(EntityKind kind) const {
    std::vector<const PropertySection*> out;
    for (const auto& s : sections_)
        if (s.kind == kind)
            out.push_back(&s);
    return out;
}

const std::vector<EntityId>& selectedWhere(const PanelContext& pc, const char* tag,
                                           bool (*keep)(const Document&, EntityId)) {
    struct Cache {
        std::uint64_t doc = 0;
        std::uint64_t revision = 0;
        const std::vector<EntityId>* selection = nullptr;
        std::size_t size = 0;
        std::uint64_t selectionRevision = 0;
        std::vector<EntityId> ids{};
    };
    static std::map<std::string, Cache> caches;
    Cache& c = caches[tag];
    static const std::vector<EntityId> none;
    if (!pc.doc || !pc.selection) {
        c = Cache{};
        return none;
    }
    const bool same = pc.selectionRevision != 0 && c.doc == pc.doc->uid() &&
                      c.revision == pc.doc->revision() && c.selection == pc.selection &&
                      c.size == pc.selection->size() && c.selectionRevision == pc.selectionRevision;
    if (!same) {
        c.doc = pc.doc->uid();
        c.revision = pc.doc->revision();
        c.selection = pc.selection;
        c.size = pc.selection->size();
        c.selectionRevision = pc.selectionRevision;
        c.ids.clear();
        for (EntityId id : *pc.selection)
            if (keep(*pc.doc, id))
                c.ids.push_back(id);
    }
    return c.ids;
}

}  // namespace tbcad::ui
