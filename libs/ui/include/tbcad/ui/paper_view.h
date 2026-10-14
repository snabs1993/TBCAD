#pragma once

#include <deque>
#include <string>
#include <vector>

#include "tbcad/core/core.h"
#include "tbcad/render/scene.h"

namespace tbcad::ui {

// A read-only picture of one layout (paper space): the white sheet, the layout's own objects and
// every viewport showing model space, clipped to its rectangle. Built from the same primitives
// the plotter uses, then fed to a render::Scene so the window draws it like a drawing. The
// coordinates are millimetres on the sheet.
class PaperView {
public:
    // Rebuilds the scene; returns an error message, or empty when it worked.
    std::string build(const Document& doc, const std::string& layoutName);
    void clear();

    render::Scene& scene() { return scene_; }
    const render::Scene& scene() const { return scene_; }
    // The sheet, from {0,0} to its size; empty before a successful build.
    const BBox2& sheet() const { return sheet_; }
    const std::string& layout() const { return layout_; }

private:
    render::Scene scene_;
    BBox2 sheet_{};
    std::string layout_{};
    std::deque<std::vector<double>> patterns_{};  // dash patterns the scene's styles point to
};

}  // namespace tbcad::ui
