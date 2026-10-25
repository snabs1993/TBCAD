#pragma once

#include <string>
#include <vector>

#include "tbcad/core/core.h"

namespace tbcad::ui {

// A small line picture of a drawing or a layout for the tab hover popup (QVDRAWING / QVLAYOUT).
// Points are scaled so the longer side is 1; x runs 0..width and y 0..height with y up.
struct TabPreview {
    std::vector<std::vector<Vec2>> strokes{};
    double width = 0;
    double height = 0;
    bool empty() const { return strokes.empty(); }
};

// The visible model space of a drawing.
TabPreview previewOfModel(const Document& doc);
// The sheet of a layout: its outline, its own objects and, inside each viewport, the model as the
// viewport shows it (scale and centre; frozen layers are not applied). Empty for an unknown name.
TabPreview previewOfLayout(const Document& doc, const std::string& layoutName);

}  // namespace tbcad::ui
