#pragma once

#include <imgui.h>

#include <string>
#include <vector>

#include "tbcad/core/color.h"
#include "tbcad/ui/theme.h"

namespace tbcad::app {

// Draws the radial tool wheel and tracks which slice is under the pointer. The Gui decides when
// it opens (right button held past the delay) and runs the chosen command on release; slot
// contents live in the `ui.wheel.tools` setting (see tbcad/ui/radial_wheel.h).
class WheelMenu {
public:
    bool isOpen() const { return open_; }
    // Opens the wheel centred at `centre` (window points) with these slots (empty = unused).
    void open(ImVec2 centre, std::vector<std::string> tools);
    void close() { open_ = false; }
    // Draws the wheel and updates the hovered slice from `mouse`. `scale` is the UI scale.
    void draw(ImVec2 mouse, float scale, const ui::Theme& theme);
    // Command of the slice under the pointer, or empty (dead zone or an unused slot).
    std::string hovered() const;

private:
    bool open_ = false;
    ImVec2 centre_{};
    std::vector<std::string> tools_{};
    int hover_ = -1;
};

}  // namespace tbcad::app
