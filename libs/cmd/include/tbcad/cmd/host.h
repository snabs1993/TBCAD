#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "tbcad/cmd/input.h"
#include "tbcad/core/core.h"

namespace tbcad::cmd {

struct ViewState {
    Vec2 center{};     // world point at the middle of the viewport
    double scale = 1;  // millimetres per screen pixel
    Vec2 sizePx{};
};

// What the command line should show while a command waits for input.
struct PromptInfo {
    enum class Kind { Point, Distance, Angle, Real, Integer, String, Keyword, Selection, Entity };
    Kind kind = Kind::Point;
    std::string text{};                   // "Specify next point"
    std::vector<std::string> keywords{};  // "Close", "Undo"
    std::optional<std::string> defaultText{};
    std::string command{};  // name of the running command

    // "Specify next point or [Close/Undo]: "
    std::string display() const;
};

// Implemented by the application (and by ScriptHost for tests). Everything the command framework
// needs from the outside world.
class IHost {
public:
    virtual ~IHost() = default;

    // Message log / command line history.
    virtual void print(std::string_view message) = 0;

    // Current selection (noun-verb). Commands read it through getSelection.
    virtual const std::vector<EntityId>& selection() const = 0;
    virtual void setSelection(std::vector<EntityId> ids) = 0;

    // Free-form settings (SNAP/GRID/ORTHO toggles ...). Keys are lower-case dotted names.
    virtual std::optional<std::string> setting(std::string_view /*key*/) const { return {}; }
    virtual void setSetting(std::string_view /*key*/, std::string /*value*/) {}

    // Folder holding aliases.json, keybindings.json ... Empty = none.
    virtual std::filesystem::path settingsFolder() const { return {}; }

    // View. A host without a viewport keeps the defaults.
    virtual ViewState view() const { return {}; }
    virtual void setView(const ViewState& /*view*/) {}

    // Ids of the model-space entities whose bounding box meets `window`, answered from the host's
    // spatial index. nullopt means the host has no index; callers then scan the document. The
    // order is unspecified. Hidden entities and layer visibility are not applied.
    virtual std::optional<std::vector<EntityId>> entitiesInWindow(const BBox2& /*window*/) const {
        return std::nullopt;
    }

    // Rubber-band preview from the active prompt; nullptr clears it.
    virtual void setPreview(const DisplayList* /*list*/) {}

    // The active prompt changed; nullptr = no command running. Snap/tracking feedback for the
    // overlay comes with it.
    virtual void promptChanged(const PromptInfo* /*prompt*/) {}
    virtual void feedbackChanged(const FilterFeedback& /*feedback*/) {}
};

}  // namespace tbcad::cmd
