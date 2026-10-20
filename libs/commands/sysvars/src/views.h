#pragma once

// Named views (VIEW / -VIEW). A view is kept in the drawing as Header::custom
// "tbcad.view.<name>" = "cx cy pixelSize [width height]" plus TAB-separated flags: "L" when a
// layer snapshot was saved with it (the hidden layer state "*view|<name>"), "C<text>" for its
// category. Old views have only the first three numbers.
#include <optional>
#include <string>
#include <vector>

#include "tbcad/core/core.h"

namespace tbcad::sysvars {

struct NamedView {
    std::string name{};
    Vec2 center{};
    double scale = 0;  // millimetres per pixel when it was saved
    double width = 0;  // model area shown (0 = not known: restore keeps `scale`)
    double height = 0;
    bool layers = false;  // a layer snapshot goes with it
    std::string category{};
};

inline constexpr const char* kViewPrefix = "tbcad.view.";
// Setting: save a layer snapshot with new views ("1", the default, or "0"), like AutoCAD's
// "Save layer snapshot with view".
inline constexpr const char* kViewLayerSnapshotKey = "view.layerSnapshot";

std::vector<NamedView> namedViews(const Document& doc);
std::optional<NamedView> findNamedView(const Document& doc, const std::string& name);
std::string encodeNamedView(const NamedView& v);
std::string viewLayerStateName(const std::string& view);

}  // namespace tbcad::sysvars
