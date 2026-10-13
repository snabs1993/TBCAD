#pragma once

#include <imgui.h>

namespace tbcad::app {

// Tool-strip icons, drawn with line primitives on a 20 x 20 grid: single colour, 1.5 px stroke at
// 100 % scale, so they follow the theme and every DPI without image files.
enum class Icon {
    New,
    Open,
    Save,
    Undo,
    Redo,
    Line,
    Polyline,
    Circle,
    Arc,
    Rectangle,
    Hatch,
    Text,
    Move,
    Copy,
    Rotate,
    Scale,
    Mirror,
    Stretch,
    Trim,
    Offset,
    Fillet,
    Erase,
    ZoomExtents,
    Count
};

// Draws `icon` centred in the square [min, min + size).
void drawIcon(ImDrawList* dl, Icon icon, ImVec2 min, float size, ImU32 color);

// An icon button of the given square size (frame included); true when clicked.
bool iconButton(const char* id, Icon icon, float size, bool enabled = true);

}  // namespace tbcad::app
