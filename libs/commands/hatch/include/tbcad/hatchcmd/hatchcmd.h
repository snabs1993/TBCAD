#pragma once

#include <string>
#include <vector>

#include "tbcad/cmd/registry.h"
#include "tbcad/core/core.h"

namespace tbcad::ui {
class UiRegistry;
}

namespace tbcad::hatchcmd {

// HATCH [H], HATCHEDIT [HE]. Also registers the Hatch display generator with geom.
void registerCommands(cmd::Registry& registry);

// Hatch panel, property section and menu items. Defined only in application builds
// (TBCAD_BUILD_APP); the generated registration calls it only then.
void registerUi(ui::UiRegistry& ui);

// Settings keys shared by the commands and the UI (host settings, lower-case dotted names).
inline constexpr const char* kSettingPattern = "hatch.pattern";  // pattern name
inline constexpr const char* kSettingScale = "hatch.scale";      // number
inline constexpr const char* kSettingAngle = "hatch.angle";      // degrees
inline constexpr const char* kSettingBase = "hatch.base";        // "x,y"
inline constexpr const char* kSettingGap = "hatch.gap";          // gap tolerance in mm, 0 = off
inline constexpr const char* kSettingAssociative = "hatch.associative";  // "1" / "0", default "1"

}  // namespace tbcad::hatchcmd
