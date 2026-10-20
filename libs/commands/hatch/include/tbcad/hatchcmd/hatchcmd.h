#pragma once

#include <string>
#include <vector>

#include "tbcad/cmd/registry.h"
#include "tbcad/core/core.h"

namespace tbcad::ui {
class UiRegistry;
}

namespace tbcad::hatchcmd {

// HATCH [H], GRADIENT [GR], HATCHEDIT [HE]. Also registers the Hatch display generator with geom.
void registerCommands(cmd::Registry& registry);

// Hatch panel, property section and menu items. Defined only in application builds
// (TBCAD_BUILD_APP); the generated registration calls it only then.
void registerUi(ui::UiRegistry& ui);

// Settings keys shared by the commands and the UI (host settings, lower-case dotted names).
inline constexpr const char* kSettingPattern = "hatch.pattern";                 // pattern name
inline constexpr const char* kSettingScale = "hatch.scale";                     // number
inline constexpr const char* kSettingAngle = "hatch.angle";                     // degrees
inline constexpr const char* kSettingBase = "hatch.base";                       // "x,y"
inline constexpr const char* kSettingGradientKind = "hatch.gradient.kind";  // LINEAR, CYLINDER..
inline constexpr const char* kSettingGradientColor1 = "hatch.gradient.color1";  // "r,g,b"
inline constexpr const char* kSettingGradientColor2 = "hatch.gradient.color2";  // "r,g,b"
inline constexpr const char* kSettingGap = "hatch.gap";  // gap tolerance in mm, 0 = off
inline constexpr const char* kSettingIsland =
    "hatch.island";  // "normal" (default), "outer", "ignore"
inline constexpr const char* kSettingAssociative = "hatch.associative";  // "1" / "0", default "1"
// "1": every area picked in one HATCH makes its own hatch (HPSEPARATE); "0" (default) one hatch.
inline constexpr const char* kSettingSeparate = "hatch.separate";
// Where a new hatch goes in the draw order (HPDRAWORDER): "none", "back", "front", "behind"
// (behind its boundary, the default) or "infront".
inline constexpr const char* kSettingDrawOrder = "hatch.drawOrder";
inline constexpr const char* kSettingAnnotative = "hatch.annotative";  // "1" / "0"
inline constexpr const char* kSettingDouble = "hatch.double";          // "1" / "0"
// New hatches' transparency: "ByLayer" (default), "ByBlock" or a percentage 0-90.
inline constexpr const char* kSettingTransparency = "hatch.transparency";
inline constexpr const char* kSettingGradientShift = "hatch.gradient.shift";  // 0 = centred
inline constexpr const char* kSettingGradientOneColor = "hatch.gradient.oneColor";  // "1" / "0"
inline constexpr const char* kSettingGradientTint = "hatch.gradient.tint";  // 0 black .. 1 white

}  // namespace tbcad::hatchcmd
