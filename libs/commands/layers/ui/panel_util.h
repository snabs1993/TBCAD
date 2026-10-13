#pragma once

#include <imgui.h>

#include <string>
#include <vector>

#include "tbcad/core/core.h"
#include "tbcad/ui/ui_registry.h"

namespace tbcad::layers::ui_detail {

// Panels change the drawing by running commands, exactly as if the user had typed them: every
// edit is one undo step and shows up in the command history, so the panel teaches the command
// line. `lines` are fed one after the other (the first starts the command, later ones answer
// its prompts as whole lines, so names may contain spaces); an empty line ends the command.
inline void exec(tbcad::ui::PanelContext& ctx, const std::vector<std::string>& lines) {
    if (!ctx.runCommand)
        return;
    for (const std::string& l : lines)
        ctx.runCommand(l);
    ctx.runCommand("");
}

inline std::string idRef(LayerId id) {
    return "#" + std::to_string(id.value);
}

inline ImVec4 swatch(Color c) {
    Rgba rgba = c.kind == ColorKind::Rgb ? Rgba{c.r, c.g, c.b, 255} : paletteColor(c.indexValue());
    // White is drawn dark on light backgrounds by the renderer; in the dark UI it stays white.
    return ImVec4(rgba.r / 255.0f, rgba.g / 255.0f, rgba.b / 255.0f, 1.0f);
}

// Colour popup: 255-index palette grid, RGB picker, ByLayer / ByBlock / ByParent buttons.
// Returns true and fills `chosen` (as the text -LAYER / -PROP accept) when a colour was picked.
struct ColorChoices {
    bool byLayer = false;
    bool byBlock = false;
    bool byParent = false;
};
bool colorPopup(const char* popupId, Color current, const ColorChoices& allowed,
                std::string& chosen);

}  // namespace tbcad::layers::ui_detail
